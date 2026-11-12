fn main() {
    if !cordgeist::native::verify_abi() {
        eprintln!(
            "Native ABI mismatch: expected pinned libaoahid 2.0.3 and matching C/Rust layouts"
        );
        std::process::exit(1);
    }
    println!("libaoahid 2.0.3 ABI matches; software-only check, no USB opened");
}
