#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    nativeusb_bench_lib::run();
}
