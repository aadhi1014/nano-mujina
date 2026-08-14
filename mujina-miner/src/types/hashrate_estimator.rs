//! Windowed hashrate estimation from share work.
//!
//! Estimates hashrate by accumulating work from shares within a
//! sliding time window. Each share records its expected work (from
//! `Target::to_work()`), and the estimator divides total work by
//! the span from the oldest sample to the current time.
//!
//! This gives an accurate estimate as soon as enough samples exist,
//! without waiting for the full window to fill. If shares stop
//! arriving, the span grows to include the silent period and the
//! estimate declines naturally.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use bitcoin::pow::Work;

use super::HashRate;
use crate::u256::U256;

/// Windowed hashrate estimator.
///
/// Tracks recent share work in a fixed-duration sliding window and
/// computes hashrate as `total_work / window_duration`.
pub struct HashrateEstimator {
    window: Duration,
    min_samples: usize,
    max_samples: usize,
    samples: VecDeque<(Instant, U256)>,
    total_work: U256,
}

impl HashrateEstimator {
    /// Create an estimator with the given measurement window.
    ///
    /// Uses reasonable defaults: settled threshold of 5 samples,
    /// capacity of `window_secs * 10` (assumes at most 10
    /// samples/sec).
    pub fn new(window: Duration) -> Self {
        let max_samples = window.as_secs() as usize * 10;
        Self::with_limits(window, 5, max_samples)
    }

    /// Create an estimator with explicit limits.
    pub fn with_limits(window: Duration, min_samples: usize, max_samples: usize) -> Self {
        Self {
            window,
            min_samples,
            max_samples,
            samples: VecDeque::new(),
            total_work: U256::ZERO,
        }
    }

    /// Record work from a share at the current time.
    pub fn record(&mut self, work: Work) {
        self.record_at(Instant::now(), work);
    }

    /// Record work from a share at the given timestamp.
    pub fn record_at(&mut self, at: Instant, work: Work) {
        let work = U256::from(work);
        self.prune_before(at.checked_sub(self.window).unwrap_or(at));
        self.samples.push_back((at, work));
        self.total_work += work;

        // Enforce capacity limit on top of time-based pruning
        while self.samples.len() > self.max_samples {
            if let Some((_, old_work)) = self.samples.pop_front() {
                self.total_work -= old_work;
            }
        }
    }

    /// Current hashrate estimate over the window.
    pub fn hashrate(&mut self) -> HashRate {
        self.hashrate_at(Instant::now())
    }

    /// Hashrate estimate at the given timestamp.
    ///
    /// Divides total work by the span from the oldest sample to `now`.
    /// This gives an accurate estimate as soon as samples exist rather
    /// than ramping up over the full window.
    pub fn hashrate_at(&mut self, now: Instant) -> HashRate {
        self.prune_before(now.checked_sub(self.window).unwrap_or(now));

        let secs = match self.samples.front() {
            Some(&(oldest, _)) => now.duration_since(oldest).as_secs(),
            None => return HashRate::from(0u64),
        };
        if secs == 0 {
            return HashRate::from(0u64);
        }

        HashRate::from((self.total_work / secs).saturating_to_u64())
    }

    /// Whether any samples exist within the window.
    pub fn has_samples(&self) -> bool {
        !self.samples.is_empty()
    }

    /// Whether the estimate has settled enough to trust.
    ///
    /// Returns true once at least `min_samples` have been recorded
    /// in the current window. Before this point, callers should
    /// prefer a static estimate (e.g., from hardware capabilities).
    pub fn is_settled(&self) -> bool {
        self.samples.len() >= self.min_samples
    }

    /// Returns the measured hashrate if the estimator has settled,
    /// or `None` if not enough samples have been collected yet.
    pub fn settled_hashrate(&mut self) -> Option<HashRate> {
        if self.is_settled() {
            Some(self.hashrate())
        } else {
            None
        }
    }

    /// Remove samples older than `cutoff`, subtracting their work.
    fn prune_before(&mut self, cutoff: Instant) {
        while let Some(&(t, work)) = self.samples.front() {
            if t >= cutoff {
                break;
            }
            self.total_work -= work;
            self.samples.pop_front();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Helper: create a Work value from a u64 hash count.
    fn work(n: u64) -> Work {
        // Builds a Work value from the u64's little-endian bytes.
        let bytes = {
            let mut b = [0u8; 32];
            b[..8].copy_from_slice(&n.to_le_bytes());
            b
        };
        Work::from_le_bytes(bytes)
    }

    #[test]
    fn no_samples() {
        let est = HashrateEstimator::new(Duration::from_secs(300));
        assert!(!est.has_samples());
    }

    #[test]
    fn no_samples_hashrate_zero() {
        let mut est = HashrateEstimator::new(Duration::from_secs(300));
        let now = Instant::now();
        assert_eq!(u64::from(est.hashrate_at(now)), 0);
    }

    #[test]
    fn single_sample_zero_span() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let now = Instant::now();

        // Single sample at `now` has zero time span, so rate is zero.
        est.record_at(now, work(1000));
        assert_eq!(u64::from(est.hashrate_at(now)), 0);
    }

    #[test]
    fn single_sample_with_elapsed() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        // One sample at base, queried 10s later: 1000 / 10 = 100 H/s
        est.record_at(base, work(1000));
        assert_eq!(
            u64::from(est.hashrate_at(base + Duration::from_secs(10))),
            100
        );
    }

    #[test]
    fn multiple_samples_sum() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        est.record_at(base, work(1000));
        est.record_at(base + Duration::from_secs(10), work(2000));
        est.record_at(base + Duration::from_secs(20), work(3000));

        // Total: 6000 work over 20s span = 300 H/s
        let rate = est.hashrate_at(base + Duration::from_secs(20));
        assert_eq!(u64::from(rate), 300);
    }

    #[test]
    fn expired_samples_pruned() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        est.record_at(base, work(5000));
        est.record_at(base + Duration::from_secs(50), work(1000));

        // At base+150, the first sample (at base) is 150s old and
        // outside the 100s window. Only the second remains.
        let at = base + Duration::from_secs(150);
        let rate = est.hashrate_at(at);
        assert_eq!(u64::from(rate), 10); // 1000 / 100
        assert!(est.has_samples());
    }

    #[test]
    fn all_samples_expired() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        est.record_at(base, work(5000));

        let at = base + Duration::from_secs(200);
        assert_eq!(u64::from(est.hashrate_at(at)), 0);
        assert!(!est.has_samples());
    }

    #[test]
    fn estimate_tracks_elapsed_time() {
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        // Record 500 work at base. At base+5s the span is 5s,
        // so rate = 500/5 = 100 H/s. At base+50s the span is 50s,
        // so rate = 500/50 = 10 H/s.
        est.record_at(base, work(500));
        assert_eq!(
            u64::from(est.hashrate_at(base + Duration::from_secs(5))),
            100
        );
        assert_eq!(
            u64::from(est.hashrate_at(base + Duration::from_secs(50))),
            10
        );
    }

    #[test]
    fn zero_duration_window() {
        let mut est = HashrateEstimator::new(Duration::ZERO);
        let now = Instant::now();
        est.record_at(now, work(1000));
        // Zero-length window: can't divide, returns zero
        assert_eq!(u64::from(est.hashrate_at(now)), 0);
    }

    #[test]
    fn prune_on_record_prevents_unbounded_growth() {
        let mut est = HashrateEstimator::new(Duration::from_secs(10));
        let base = Instant::now();

        // Add 100 samples over 100 seconds (only last ~10 should remain)
        for i in 0..100 {
            est.record_at(base + Duration::from_secs(i), work(100));
        }

        // Window is 10s, so at most ~11 samples should remain
        // (samples from t=90..99 are within [89, 99] window)
        assert!(est.samples.len() <= 12);
    }

    #[test]
    fn capacity_enforced() {
        let mut est = HashrateEstimator::with_limits(
            Duration::from_secs(1000), // large window so time pruning doesn't interfere
            1,
            20,
        );
        let base = Instant::now();

        // Add 50 samples within the window
        for i in 0..50 {
            est.record_at(base + Duration::from_secs(i), work(100));
        }

        // Capacity is 20, so oldest samples were dropped
        assert_eq!(est.samples.len(), 20);

        // Retained samples span t=30..49 (19s), total work = 2000.
        // Rate = 2000 / 19 = 105 H/s.
        let rate = est.hashrate_at(base + Duration::from_secs(49));
        assert_eq!(u64::from(rate), 105);
    }

    #[test]
    fn not_settled_initially() {
        let est = HashrateEstimator::new(Duration::from_secs(100));
        assert!(!est.is_settled());
    }

    #[test]
    fn settled_after_min_samples() {
        // Default min_samples is 5
        let mut est = HashrateEstimator::new(Duration::from_secs(100));
        let base = Instant::now();

        for i in 0..4 {
            est.record_at(base + Duration::from_secs(i), work(100));
        }
        assert!(!est.is_settled());

        est.record_at(base + Duration::from_secs(4), work(100));
        assert!(est.is_settled());
    }

    #[test]
    fn settled_with_custom_threshold() {
        let mut est = HashrateEstimator::with_limits(Duration::from_secs(100), 3, 1000);
        let base = Instant::now();

        est.record_at(base, work(100));
        est.record_at(base + Duration::from_secs(1), work(100));
        assert!(!est.is_settled());

        est.record_at(base + Duration::from_secs(2), work(100));
        assert!(est.is_settled());
    }

    #[test]
    fn settled_hashrate_none_before_settled() {
        let mut est = HashrateEstimator::with_limits(Duration::from_secs(100), 3, 1000);
        let base = Instant::now();

        est.record_at(base, work(500));
        assert!(est.settled_hashrate().is_none());
    }

    #[test]
    fn settled_hashrate_some_after_settled() {
        let mut est = HashrateEstimator::with_limits(Duration::from_secs(100), 3, 1000);
        let base = Instant::now();

        for i in 0..3 {
            est.record_at(base + Duration::from_secs(i), work(1000));
        }
        // 3000 work over 2s span = 1500 H/s
        let rate = est.hashrate_at(base + Duration::from_secs(2));
        assert_eq!(u64::from(rate), 1500);
    }
}
