//! Command-line interface for mujina-miner.
//!
//! This binary provides a CLI for controlling and monitoring the miner
//! daemon via the HTTP API.

use std::env;

use anyhow::Result;

use mujina_miner::api_client;

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        eprintln!("Usage: mujina-cli <command> [args]");
        eprintln!();
        eprintln!("Commands:");
        eprintln!("  status          Show miner status");
        eprintln!("  api <endpoint>  Raw API call (e.g. \"api miner\")");
        eprintln!();
        eprintln!("Environment:");
        eprintln!("  MUJINA_API_URL    API base URL (default: http://127.0.0.1:7785)");
        std::process::exit(1);
    }

    let command = &args[1];

    match command.as_str() {
        "status" => cmd_status().await?,
        "api" => {
            let endpoint = args.get(2).map_or("", String::as_str);
            cmd_api(endpoint).await?;
        }
        _ => {
            eprintln!("Unknown command: {}", command);
            eprintln!("Run without arguments to see usage.");
            std::process::exit(1);
        }
    }

    Ok(())
}

/// Build an API client, honoring MUJINA_API_URL if set.
fn make_client() -> api_client::Client {
    match env::var("MUJINA_API_URL") {
        Ok(url) => api_client::Client::with_base_url(url),
        Err(_) => api_client::Client::new(),
    }
}

/// Make a raw API call and pretty-print the JSON response.
async fn cmd_api(endpoint: &str) -> Result<()> {
    let client = make_client();
    let body = client.get_raw(endpoint).await?;

    // Try to pretty-print as JSON; fall back to raw text
    match serde_json::from_str::<serde_json::Value>(&body) {
        Ok(json) => println!("{}", serde_json::to_string_pretty(&json)?),
        Err(_) => print!("{}", body),
    }

    Ok(())
}

/// Print a summary of the current miner state.
async fn cmd_status() -> Result<()> {
    let client = make_client();
    let state = client.get_miner().await?;

    println!("Uptime:  {} s", state.uptime_secs);
    println!("Hashrate: {} H/s", state.hashrate);
    println!("Shares:  {}", state.shares_submitted);

    if state.sources.is_empty() {
        println!("Sources: (none)");
    } else {
        println!("Sources:");
        for source in &state.sources {
            println!("  - {}", source.name);
        }
    }

    if !state.boards.is_empty() {
        println!("Boards:");
        for board in &state.boards {
            println!("  - {}", board.model);
        }
    }

    Ok(())
}
