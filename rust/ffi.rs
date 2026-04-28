use core::ffi::c_char;

unsafe extern "C" {
    #[link_name = "kprintf"]
    pub fn kprintf_raw(fmt: *const c_char, ...);
}

#[macro_export]
macro_rules! kprintf {
    ($fmt:expr $(, $arg:expr)* $(,)?) => {{
        unsafe {
            $crate::ffi::kprintf_raw($fmt.as_ptr() $(, $arg)*);
        }
    }};
}
