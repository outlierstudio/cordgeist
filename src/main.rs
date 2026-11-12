fn main() {
    let code = cordgeist::cli_app::run();
    let _ = std::io::Write::flush(&mut std::io::stdout());
    let _ = std::io::Write::flush(&mut std::io::stderr());
    std::process::exit(code);
}
