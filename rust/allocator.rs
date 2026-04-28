use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::mem;
use core::ptr::{self, NonNull};

use crate::resource;

const HEADER_SIZE: usize = mem::size_of::<usize>();

struct KernelAllocator;

#[global_allocator]
static GLOBAL_ALLOCATOR: KernelAllocator = KernelAllocator;

#[inline]
const fn align_up(value: usize, align: usize) -> usize {
    (value + (align - 1)) & !(align - 1)
}

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.size() == 0 {
            return layout.align() as *mut u8;
        }

        let align = layout.align().max(mem::align_of::<usize>());
        let total = match layout
            .size()
            .checked_add(align)
            .and_then(|n| n.checked_add(HEADER_SIZE))
        {
            Some(v) => v,
            None => return ptr::null_mut(),
        };

        let raw = match resource::kmalloc(total) {
            Some(p) => p.as_ptr() as usize,
            None => return ptr::null_mut(),
        };

        let aligned = align_up(raw + HEADER_SIZE, align);
        let header_ptr = (aligned - HEADER_SIZE) as *mut usize;
        header_ptr.write(raw);

        aligned as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if ptr.is_null() || layout.size() == 0 {
            return;
        }

        let header_ptr = (ptr as usize - HEADER_SIZE) as *const usize;
        let raw = header_ptr.read() as *mut c_void;

        resource::kfree(NonNull::new_unchecked(raw));
    }
}
