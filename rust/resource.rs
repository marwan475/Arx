use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr::NonNull;

pub const DEBUG: bool = cfg!(debug_assertions);

unsafe extern "C" {
    #[link_name = "kprintf"]
    fn kprintf_raw(fmt: *const c_char, ...);

    fn kterm_write_raw(msg: *const c_char);
    #[link_name = "kterm_printf"]
    fn kterm_printf_raw(fmt: *const c_char, ...) -> c_int;

    #[link_name = "memset"]
    fn memset_raw(dest: *mut c_void, value: c_int, count: usize) -> *mut c_void;
    #[link_name = "memcpy"]
    fn memcpy_raw(dest: *mut c_void, src: *const c_void, count: usize) -> *mut c_void;
    #[link_name = "memcmp"]
    fn memcmp_raw(lhs: *const c_void, rhs: *const c_void, count: usize) -> c_int;
    #[link_name = "strlen"]
    fn strlen_raw(str: *const c_char) -> usize;

    #[link_name = "pa_to_hhdm"]
    fn pa_to_hhdm_raw(pa: usize, hhdm_present: bool, hhdm_offset: u64) -> usize;
    #[link_name = "hhdm_to_pa"]
    fn hhdm_to_pa_raw(hhdm_addr: usize, hhdm_present: bool, hhdm_offset: u64) -> usize;

    #[link_name = "vmalloc"]
    fn vmalloc_raw(size: usize) -> *mut c_void;
    #[link_name = "vfree"]
    fn vfree_raw(ptr: *mut c_void);

    #[link_name = "kmalloc"]
    fn kmalloc_raw(size: usize) -> *mut c_void;
    #[link_name = "kzalloc"]
    fn kzalloc_raw(size: usize) -> *mut c_void;
    #[link_name = "kfree"]
    fn kfree_raw(ptr: *mut c_void);
}

pub fn kprintf(fmt: &CStr, value: u64) {
    unsafe {
        kprintf_raw(fmt.as_ptr(), value);
    }
}

pub fn kterm_write(msg: &CStr) {
    unsafe {
        kterm_write_raw(msg.as_ptr());
    }
}

pub fn kterm_printf(fmt: &CStr, value: u64) -> c_int {
    unsafe { kterm_printf_raw(fmt.as_ptr(), value) }
}

pub fn kdebug_write(msg: &CStr) {
    if DEBUG {
        unsafe {
            kterm_printf_raw(c"\x1b[32m[debug]\x1b[0m ".as_ptr());
            kterm_write_raw(msg.as_ptr());
        }
    }
}

pub fn kdebug_printf(fmt: &CStr, value: u64) -> c_int {
    if DEBUG {
        unsafe {
            kterm_printf_raw(c"\x1b[32m[debug]\x1b[0m ".as_ptr());
            return kterm_printf_raw(fmt.as_ptr(), value);
        }
    }

    0
}

#[macro_export]
macro_rules! KDEBUG {
    ($msg:expr) => {{
        $crate::resource::kdebug_write($msg);
    }};
    ($fmt:expr, $value:expr) => {{
        $crate::resource::kdebug_printf($fmt, $value as u64);
    }};
}

pub fn memset_bytes(dest: &mut [u8], value: u8) {
    unsafe {
        memset_raw(dest.as_mut_ptr().cast(), value as c_int, dest.len());
    }
}

pub fn memcpy_bytes(dest: &mut [u8], src: &[u8]) {
    assert!(dest.len() >= src.len(), "destination slice is smaller than source slice");
    unsafe {
        memcpy_raw(dest.as_mut_ptr().cast(), src.as_ptr().cast(), src.len());
    }
}

pub fn memcmp_bytes(lhs: &[u8], rhs: &[u8]) -> c_int {
    assert!(lhs.len() == rhs.len(), "memcmp requires equal slice lengths");
    unsafe { memcmp_raw(lhs.as_ptr().cast(), rhs.as_ptr().cast(), lhs.len()) }
}

pub fn strlen(cstr: &CStr) -> usize {
    unsafe { strlen_raw(cstr.as_ptr()) }
}

pub fn pa_to_hhdm(pa: usize, hhdm_present: bool, hhdm_offset: u64) -> usize {
    unsafe { pa_to_hhdm_raw(pa, hhdm_present, hhdm_offset) }
}

pub fn hhdm_to_pa(hhdm_addr: usize, hhdm_present: bool, hhdm_offset: u64) -> usize {
    unsafe { hhdm_to_pa_raw(hhdm_addr, hhdm_present, hhdm_offset) }
}

pub fn vmalloc(size: usize) -> Option<NonNull<c_void>> {
    NonNull::new(unsafe { vmalloc_raw(size) })
}

pub fn vfree(ptr: NonNull<c_void>) {
    unsafe {
        vfree_raw(ptr.as_ptr());
    }
}

pub fn kmalloc(size: usize) -> Option<NonNull<c_void>> {
    NonNull::new(unsafe { kmalloc_raw(size) })
}

pub fn kzalloc(size: usize) -> Option<NonNull<c_void>> {
    NonNull::new(unsafe { kzalloc_raw(size) })
}

pub fn kfree(ptr: NonNull<c_void>) {
    unsafe {
        kfree_raw(ptr.as_ptr());
    }
}
