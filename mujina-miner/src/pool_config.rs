//! Persisted pool configuration, read at daemon startup and writable live
//! via `PATCH /api/v0/pool` without editing the boot script by hand.
//!
//! Stored at `/data/userconfig/pool.conf`, the same persistent partition
//! `bin/ble_setup.rs` uses for WiFi credentials (`wpa_supplicant.conf`).
//! Plain `key=value` lines -- `url`, `user`, `pass` -- to match that
//! file's own simplicity rather than pull in a TOML/JSON dependency for
//! three fields.
//!
//! Precedence at startup (see `daemon.rs`): a field set here overrides the
//! `MUJINA_POOL_*` env vars baked into the boot script; any field left
//! unset here falls back to the env var, then to the daemon's own
//! hardcoded default. The stratum client has no hot-reload path, so a
//! saved change here only takes effect on the miner's next restart.

use std::io;
use std::path::Path;

pub const POOL_CONFIG_PATH: &str = "/data/userconfig/pool.conf";

#[derive(Debug, Clone, Default, PartialEq)]
pub struct PoolConfig {
    pub url: Option<String>,
    pub user: Option<String>,
    pub pass: Option<String>,
}

/// Reads the persisted pool config file, if present. A missing file or an
/// unparseable/blank line is treated as "no override" for that field, not
/// an error -- callers fall back to env vars/defaults regardless.
pub fn load() -> PoolConfig {
    load_from(Path::new(POOL_CONFIG_PATH))
}

/// Merges the given fields onto the currently persisted config (`None`
/// keeps the current persisted value for that field) and writes the
/// result, returning the merged config.
pub fn save_merged(
    url: Option<&str>,
    user: Option<&str>,
    pass: Option<&str>,
) -> io::Result<PoolConfig> {
    save_merged_to(Path::new(POOL_CONFIG_PATH), url, user, pass)
}

fn load_from(path: &Path) -> PoolConfig {
    let mut cfg = PoolConfig::default();
    let Ok(contents) = std::fs::read_to_string(path) else {
        return cfg;
    };
    for line in contents.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let value = value.trim();
        if value.is_empty() {
            continue;
        }
        match key.trim() {
            "url" => cfg.url = Some(value.to_string()),
            "user" => cfg.user = Some(value.to_string()),
            "pass" => cfg.pass = Some(value.to_string()),
            _ => {}
        }
    }
    cfg
}

fn save_merged_to(
    path: &Path,
    url: Option<&str>,
    user: Option<&str>,
    pass: Option<&str>,
) -> io::Result<PoolConfig> {
    let mut cfg = load_from(path);
    if let Some(url) = url {
        cfg.url = Some(url.to_string());
    }
    if let Some(user) = user {
        cfg.user = Some(user.to_string());
    }
    if let Some(pass) = pass {
        cfg.pass = Some(pass.to_string());
    }

    let mut out = String::new();
    if let Some(u) = &cfg.url {
        out.push_str(&format!("url={u}\n"));
    }
    if let Some(u) = &cfg.user {
        out.push_str(&format!("user={u}\n"));
    }
    if let Some(p) = &cfg.pass {
        out.push_str(&format!("pass={p}\n"));
    }
    std::fs::write(path, out)?;
    Ok(cfg)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_path(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("mujina_pool_config_test_{name}_{:?}", std::thread::current().id()))
    }

    #[test]
    fn missing_file_loads_as_default() {
        let path = temp_path("missing");
        let _ = std::fs::remove_file(&path);
        assert_eq!(load_from(&path), PoolConfig::default());
    }

    #[test]
    fn round_trips_all_fields() {
        let path = temp_path("roundtrip");
        let cfg = save_merged_to(
            &path,
            Some("stratum+tcp://pool:3333"),
            Some("wallet.worker"),
            Some("x"),
        )
        .unwrap();
        assert_eq!(cfg.url.as_deref(), Some("stratum+tcp://pool:3333"));
        assert_eq!(cfg.user.as_deref(), Some("wallet.worker"));
        assert_eq!(cfg.pass.as_deref(), Some("x"));
        assert_eq!(load_from(&path), cfg);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn unset_fields_keep_previous_value() {
        let path = temp_path("merge");
        save_merged_to(&path, Some("stratum+tcp://a:3333"), Some("user-a"), None).unwrap();
        let cfg = save_merged_to(&path, Some("stratum+tcp://b:3333"), None, None).unwrap();
        assert_eq!(cfg.url.as_deref(), Some("stratum+tcp://b:3333"));
        assert_eq!(cfg.user.as_deref(), Some("user-a"));
        assert_eq!(cfg.pass, None);
        let _ = std::fs::remove_file(&path);
    }
}
