#![no_std]
#![allow(non_snake_case)]
#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(clippy::too_many_arguments)]
#![allow(clippy::redundant_static_lifetimes)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

pub fn _bindgen_raw_src() -> &'static str {
    include_str!(concat!(env!("OUT_DIR"), "/bindings.rs"))
}

// string_impl (Redox OS's strlen/strcmp/etc reimplementations) is intentionally
// NOT compiled in: it's meant for bare-metal no_std targets with no real libc.
// On our real glibc-linux target its #[no_mangle] exports interpose over glibc's
// own definitions program-wide (including in std's and glibc's own pre-main
// startup code), and its unchecked pointer arithmetic on NULL/misaligned
// pointers crashes before main() ever runs. Deleting it lets glibc's own
// strlen/strcmp/etc resolve normally, which is what we want here anyway.

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn basic_sanity_check() {
        unsafe {
            lv_init();

            let horizontal_resolution = lv_disp_get_hor_res(core::ptr::null_mut());
            assert_eq!(horizontal_resolution, 0 as i16);

            let vertical_resolution = lv_disp_get_ver_res(core::ptr::null_mut());
            assert_eq!(vertical_resolution, 0 as i16);
        }
    }
}
