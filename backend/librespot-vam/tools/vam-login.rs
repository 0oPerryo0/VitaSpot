//! `vam-login` - create a librespot `credentials.json` on a PC so it can be
//! copied to `ux0:data/VitaSpot/` on a Vita where Zeroconf is unavailable.
//!
//! Usage:
//!   vam-login [--out <credentials.json>] [--cache <dir>] [--port <port>]
//!
//! The helper opens Spotify's OAuth page in the desktop browser, then logs in
//! with the resulting access token. librespot turns that into reusable stored
//! credentials and writes `<cache>/credentials.json`, which this tool copies
//! to `--out` (default: ./credentials.json).

use std::io::{self, Write};
use std::path::{Path, PathBuf};

use librespot_core::{Session, SessionConfig, authentication::Credentials, cache::Cache};
use librespot_oauth::OAuthClientBuilder;

const SCOPES: &[&str] = &[
    "app-remote-control",
    "playlist-read",
    "playlist-read-collaborative",
    "playlist-read-private",
    "streaming",
    "user-follow-modify",
    "user-follow-read",
    "user-library-modify",
    "user-library-read",
    "user-read-currently-playing",
    "user-read-playback-position",
    "user-read-playback-state",
    "user-read-recently-played",
    "user-read-private",
    "user-top-read",
];

fn usage() -> ! {
    eprintln!(
        "usage: vam-login [--out <credentials.json>] [--cache <dir>] [--port <port>]\n\
         \n\
         Logs in with Spotify's OAuth flow and writes a librespot credentials file\n\
         that can be copied to ux0:data/VitaSpot/credentials.json on the Vita."
    );
    std::process::exit(2);
}

fn main() {
    let mut out = PathBuf::from("credentials.json");
    let mut cache = PathBuf::from(".vam-login-cache");
    let mut port: u16 = 5588;

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--out" => out = PathBuf::from(args.next().unwrap_or_else(|| usage())),
            "--cache" => cache = PathBuf::from(args.next().unwrap_or_else(|| usage())),
            "--port" => {
                port = args
                    .next()
                    .and_then(|p| p.parse().ok())
                    .unwrap_or_else(|| usage())
            }
            "--help" | "-h" => usage(),
            _ => usage(),
        }
    }

    if let Err(why) = run(&out, &cache, port) {
        eprintln!("vam-login failed: {why}");
        std::process::exit(1);
    }
}

fn run(out: &Path, cache: &Path, port: u16) -> Result<(), Box<dyn std::error::Error>> {
    std::fs::create_dir_all(cache)?;
    let session_config = SessionConfig::default();
    let client_id = session_config.client_id.clone();
    let redirect = format!("http://127.0.0.1:{port}/login");

    println!("Opening the Spotify login page in your browser...");
    println!("If it does not open, visit the URL printed above the consent page.");
    let client = OAuthClientBuilder::new(&client_id, &redirect, SCOPES.to_vec())
        .open_in_browser()
        .build()?;
    let token = client.get_access_token()?;
    println!("Got an access token, logging in with librespot...");

    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    let cache_dir = cache.to_path_buf();
    runtime
        .block_on(async move {
            let cache = Cache::new(
                Some(cache_dir.clone()),
                Some(cache_dir.clone()),
                None::<PathBuf>,
                None,
            )?;
            let session = Session::new(session_config, Some(cache));
            session
                .connect(Credentials::with_access_token(token.access_token), true)
                .await?;
            println!("Logged in as {:?}", session.username());
            Ok::<(), Box<dyn std::error::Error + Send + Sync>>(())
        })
        .map_err(|e| -> Box<dyn std::error::Error> { e.to_string().into() })?;

    let stored = cache.join("credentials.json");
    if !stored.exists() {
        return Err("librespot did not write credentials.json".into());
    }
    std::fs::copy(&stored, out)?;
    println!("\nWrote {}", out.display());
    println!(
        "Copy it to the Vita as ux0:data/VitaSpot/credentials.json and restart VitaSpot."
    );
    io::stdout().flush()?;
    Ok(())
}
