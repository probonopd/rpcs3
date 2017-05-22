#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include "Emu/Cell/lv2/sys_interrupt.h"
#include "Emu/Cell/lv2/sys_lwmutex.h"
#include "Emu/Cell/lv2/sys_mutex.h"
#include "sysPrxForUser.h"

extern logs::channel sysPrxForUser;

vm::gvar<sys_lwmutex_t> g_ppu_atexit_lwm;
vm::gvar<vm::ptr<void()>[8]> g_ppu_atexit;
vm::gvar<u32> g_ppu_once_mutex;

static u32 s_tls_addr = 0; // TLS image address
static u32 s_tls_file = 0; // TLS image size
static u32 s_tls_zero = 0; // TLS zeroed area size (TLS mem size - TLS image size)
static u32 s_tls_size = 0; // Size of TLS area per thread
static u32 s_tls_area = 0; // Start of TLS memory area
static u32 s_tls_max = 0; // Max number of threads
static std::unique_ptr<atomic_t<bool>[]> s_tls_map; // I'd like to make it std::vector but it won't work

static u32 ppu_alloc_tls()
{
	u32 addr = 0;

	for (u32 i = 0; i < s_tls_max; i++)
	{
		if (!s_tls_map[i] && s_tls_map[i].exchange(true) == false)
		{
			// Default (small) TLS allocation
			addr = s_tls_area + i * s_tls_size;
			break;			
		}
	}

	if (!addr)
	{
		// Alternative (big) TLS allocation
		addr = vm::alloc(s_tls_size, vm::main);
	}

	std::memset(vm::base(addr), 0, 0x30); // Clear system area (TODO)
	std::memcpy(vm::base(addr + 0x30), vm::base(s_tls_addr), s_tls_file); // Copy TLS image
	std::memset(vm::base(addr + 0x30 + s_tls_file), 0, s_tls_zero); // Clear the rest
	return addr;
}

static void ppu_free_tls(u32 addr)
{
	// Calculate TLS position
	const u32 i = (addr - s_tls_area) / s_tls_size;

	if (addr < s_tls_area || i >= s_tls_max || (addr - s_tls_area) % s_tls_size)
	{
		// Alternative TLS allocation detected
		vm::dealloc_verbose_nothrow(addr, vm::main);
		return;
	}

	if (s_tls_map[i].exchange(false) == false)
	{
		sysPrxForUser.error("ppu_free_tls(0x%x): deallocation failed", addr);
		return;
	}
}

void sys_initialize_tls(ppu_thread& ppu, u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size, u32 tls_mem_size)
{
	sysPrxForUser.notice("sys_initialize_tls(thread_id=0x%llx, addr=*0x%x, size=0x%x, mem_size=0x%x)", main_thread_id, tls_seg_addr, tls_seg_size, tls_mem_size);

	// Uninitialized TLS expected.
	if (ppu.gpr[13] != 0) return;

	// Initialize TLS memory
	s_tls_addr = tls_seg_addr;
	s_tls_file = tls_seg_size;
	s_tls_zero = tls_mem_size - tls_seg_size;
	s_tls_size = tls_mem_size + 0x30; // 0x30 is system area size
	s_tls_area = vm::alloc(0x40000, vm::main) + 0x30;
	s_tls_max  = (0x40000 - 0x30) / s_tls_size;
	s_tls_map  = std::make_unique<atomic_t<bool>[]>(s_tls_max);

	// Allocate TLS for main thread
	ppu.gpr[13] = ppu_alloc_tls() + 0x7000 + 0x30;

	sysPrxForUser.notice("TLS initialized (addr=0x%x, size=0x%x, max=0x%x)", s_tls_area - 0x30, s_tls_size, s_tls_max);

	// TODO
	g_spu_printf_agcb = vm::null;
	g_spu_printf_dgcb = vm::null;
	g_spu_printf_atcb = vm::null;
	g_spu_printf_dtcb = vm::null;

	vm::var<sys_lwmutex_attribute_t> lwa;
	lwa->protocol   = SYS_SYNC_PRIORITY;
	lwa->recursive  = SYS_SYNC_RECURSIVE;
	lwa->name_u64   = "atexit!\0"_u64;
	sys_lwmutex_create(g_ppu_atexit_lwm, lwa);

	vm::var<sys_mutex_attribute_t> attr;
	attr->protocol  = SYS_SYNC_PRIORITY;
	attr->recursive = SYS_SYNC_NOT_RECURSIVE;
	attr->pshared   = SYS_SYNC_NOT_PROCESS_SHARED;
	attr->adaptive  = SYS_SYNC_NOT_ADAPTIVE;
	attr->ipc_key   = 0;
	attr->flags     = 0;
	attr->name_u64  = "_lv2ppu\0"_u64;
	sys_mutex_create(g_ppu_once_mutex, attr);
}

error_code sys_ppu_thread_create(ppu_thread& ppu, vm::ptr<u64> thread_id, u32 entry, u64 arg, s32 prio, u32 stacksize, u64 flags, vm::cptr<char> threadname)
{
	sysPrxForUser.warning("sys_ppu_thread_create(thread_id=*0x%x, entry=0x%x, arg=0x%llx, prio=%d, stacksize=0x%x, flags=0x%llx, threadname=%s)",
		thread_id, entry, arg, prio, stacksize, flags, threadname);

	// Allocate TLS
	const u32 tls_addr = ppu_alloc_tls();

	if (!tls_addr)
	{
		return CELL_ENOMEM;
	}

	// Call the syscall
	if (error_code res = _sys_ppu_thread_create(thread_id, vm::make_var(ppu_thread_param_t{ entry, tls_addr + 0x7030 }), arg, 0, prio, stacksize, flags, threadname))
	{
		return res;
	}

	if (flags & SYS_PPU_THREAD_CREATE_INTERRUPT)
	{
		return CELL_OK;
	}

	// Run the thread
	if (error_code res = sys_ppu_thread_start(ppu, static_cast<u32>(*thread_id)))
	{
		return res;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_id(ppu_thread& ppu, vm::ptr<u64> thread_id)
{
	sysPrxForUser.trace("sys_ppu_thread_get_id(thread_id=*0x%x)", thread_id);

	*thread_id = ppu.id;

	return CELL_OK;
}

void sys_ppu_thread_exit(ppu_thread& ppu, u64 val)
{
	sysPrxForUser.trace("sys_ppu_thread_exit(val=0x%llx)", val);

	// Call registered atexit functions
	verify(HERE), !sys_lwmutex_lock(ppu, g_ppu_atexit_lwm, 0);

	for (auto ptr : *g_ppu_atexit)
	{
		if (ptr)
		{
			ptr(ppu);
		}
	}

	verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
	
	// Deallocate TLS
	ppu_free_tls(vm::cast(ppu.gpr[13], HERE) - 0x7030);

	if (ppu.gpr[3] == val)
	{
		// Change sys_ppu_thread_exit code to the syscall code (hack)
		ppu.gpr[11] = 41;
	}

	// Call the syscall
	return _sys_ppu_thread_exit(ppu, val);
}

error_code sys_ppu_thread_register_atexit(ppu_thread& ppu, vm::ptr<void()> func)
{
	sysPrxForUser.notice("sys_ppu_thread_register_atexit(ptr=*0x%x)", func);

	verify(HERE), !sys_lwmutex_lock(ppu, g_ppu_atexit_lwm, 0);

	for (auto ptr : *g_ppu_atexit)
	{
		if (ptr == func)
		{
			verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
			return CELL_EPERM;
		}
	}

	for (auto& pp : *g_ppu_atexit)
	{
		if (pp == vm::null)
		{
			pp = func;
			verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
			return CELL_OK;
		}
	}

	verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
	return CELL_ENOMEM;
}

error_code sys_ppu_thread_unregister_atexit(ppu_thread& ppu, vm::ptr<void()> func)
{
	sysPrxForUser.notice("sys_ppu_thread_unregister_atexit(ptr=*0x%x)", func);

	verify(HERE), !sys_lwmutex_lock(ppu, g_ppu_atexit_lwm, 0);

	for (auto& pp : *g_ppu_atexit)
	{
		if (pp == func)
		{
			pp = vm::null;
			verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
			return CELL_OK;
		}
	}

	verify(HERE), !sys_lwmutex_unlock(ppu, g_ppu_atexit_lwm);
	return CELL_ESRCH;
}

void sys_ppu_thread_once(ppu_thread& ppu, vm::ptr<s32> once_ctrl, vm::ptr<void()> init)
{
	sysPrxForUser.notice("sys_ppu_thread_once(once_ctrl=*0x%x, init=*0x%x)", once_ctrl, init);

	verify(HERE), !sys_mutex_lock(ppu, *g_ppu_once_mutex, 0);

	if (*once_ctrl == SYS_PPU_THREAD_ONCE_INIT)
	{
		// Call init function using current thread context
		init(ppu);
		*once_ctrl = SYS_PPU_THREAD_DONE_INIT;
	}

	verify(HERE), !sys_mutex_unlock(ppu, *g_ppu_once_mutex);
}

error_code sys_interrupt_thread_disestablish(ppu_thread& ppu, u32 ih)
{
	sysPrxForUser.trace("sys_interrupt_thread_disestablish(ih=0x%x)", ih);

	// Recovered TLS pointer
	vm::var<u64> r13;

	// Call the syscall
	if (error_code res = _sys_interrupt_thread_disestablish(ppu, ih, r13))
	{
		return res;
	}

	// Deallocate TLS
	ppu_free_tls(vm::cast(*r13, HERE) - 0x7030);
	return CELL_OK;
}

void sysPrxForUser_sys_ppu_thread_init()
{
	// Private
	REG_VNID(sysPrxForUser, 0x00000000u, g_ppu_atexit_lwm);
	REG_VNID(sysPrxForUser, 0x00000001u, g_ppu_once_mutex);
	REG_VNID(sysPrxForUser, 0x00000002u, g_ppu_atexit);

	REG_FUNC(sysPrxForUser, sys_initialize_tls);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_create);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_get_id);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_exit);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_once);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_register_atexit);
	REG_FUNC(sysPrxForUser, sys_ppu_thread_unregister_atexit);
	REG_FUNC(sysPrxForUser, sys_interrupt_thread_disestablish);
}
