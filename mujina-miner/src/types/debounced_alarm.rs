//! A one-shot alarm that fires after a condition persists for a debounce
//! duration.
//!
//! Useful for warning about sustained problems while ignoring
//! transients that resolve on their own.
//!
//! # State Machine
//!
//! ```text
//!          check(true)              elapsed >= debounce
//!  Idle ──────────────► Timing ──────────────────────► Fired
//!   ▲                     │                              │
//!   │     check(false)    │  check(false)                │
//!   └─────────────────────┘                              │
//!   │                              check(false)          │
//!   └────────────────────────────────────────────────────┘
//!   ▲                                                    │
//!   └──────────────────── reset() ───────────────────────┘
//! ```
//!
//! - **Idle:** Condition is false (or just reset). Waiting for trouble.
//! - **Timing:** Condition is true, counting toward the debounce
//!   threshold.
//! - **Fired:** Alarm triggered. Stays here until the condition
//!   resolves or an external reset occurs.
//!
//! `check()` returns an [`AlarmStatus`] describing the transition so
//! callers can act on exactly the edges they care about (typically
//! `Triggered` and `Resolved`).

use std::time::Duration;

use tokio::time::Instant;

/// Result of [`DebouncedAlarm::check`], describing the current state
/// and any transition that just occurred.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AlarmStatus {
    /// Condition is false, alarm is idle. Nothing to do.
    Idle,

    /// Condition is true but within the debounce window. Waiting.
    Pending,

    /// Debounce elapsed -- alarm just triggered. Returned exactly
    /// once per episode; subsequent calls with a true condition
    /// return [`Active`](AlarmStatus::Active).
    Triggered,

    /// Already triggered, condition still true. Suppressed.
    Active,

    /// Was triggered, condition just became false. Returned exactly
    /// once; subsequent calls with a false condition return
    /// [`Idle`](AlarmStatus::Idle).
    Resolved,
}

/// Internal state of the alarm.
#[derive(Debug)]
enum State {
    Idle,
    Timing(Instant),
    Fired,
}

/// A one-shot alarm with debounce.
///
/// Tracks a boolean condition over time. When the condition stays true
/// for at least `debounce`, [`check`](Self::check) returns
/// [`Triggered`](AlarmStatus::Triggered) exactly once. The alarm then
/// stays in the fired state until the condition becomes false (returning
/// [`Resolved`](AlarmStatus::Resolved) once) or [`reset`](Self::reset)
/// is called.
#[derive(Debug)]
pub struct DebouncedAlarm {
    debounce: Duration,
    state: State,
}

impl DebouncedAlarm {
    /// Create a new alarm with the given debounce duration.
    pub fn new(debounce: Duration) -> Self {
        Self {
            debounce,
            state: State::Idle,
        }
    }

    /// Update the alarm with the current condition.
    ///
    /// Returns an [`AlarmStatus`] describing what happened:
    ///
    /// | Previous state | condition | Result |
    /// |----------------|-----------|--------|
    /// | Idle | false | `Idle` |
    /// | Idle | true | `Pending` (starts timer) |
    /// | Timing | false | `Idle` (resets timer) |
    /// | Timing | true | `Pending` or `Triggered` |
    /// | Fired | false | `Resolved` (re-arms) |
    /// | Fired | true | `Active` (suppressed) |
    pub fn check(&mut self, condition: bool) -> AlarmStatus {
        match (&self.state, condition) {
            (State::Idle, false) => AlarmStatus::Idle,

            (State::Idle, true) => {
                self.state = State::Timing(Instant::now());
                AlarmStatus::Pending
            }

            (State::Timing(_), false) => {
                self.state = State::Idle;
                AlarmStatus::Idle
            }

            (State::Timing(since), true) => {
                if since.elapsed() >= self.debounce {
                    self.state = State::Fired;
                    AlarmStatus::Triggered
                } else {
                    AlarmStatus::Pending
                }
            }

            (State::Fired, false) => {
                self.state = State::Idle;
                AlarmStatus::Resolved
            }

            (State::Fired, true) => AlarmStatus::Active,
        }
    }

    /// Reset the alarm to idle, regardless of current state.
    ///
    /// Use when an external event invalidates the condition (e.g.,
    /// hashrate changed, difficulty changed). Re-arms the alarm so
    /// a future episode can trigger again.
    pub fn reset(&mut self) {
        self.state = State::Idle;
    }
}

#[cfg(test)]
mod tests {
    use tokio::time;

    use super::*;

    // All tests use start_paused so Instant::now() is deterministic
    // and time::advance() controls the clock.

    #[tokio::test(start_paused = true)]
    async fn idle_stays_idle_on_false() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        assert_eq!(alarm.check(false), AlarmStatus::Idle);
        assert_eq!(alarm.check(false), AlarmStatus::Idle);
    }

    #[tokio::test(start_paused = true)]
    async fn true_starts_pending() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
    }

    #[tokio::test(start_paused = true)]
    async fn pending_clears_on_false() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
        assert_eq!(alarm.check(false), AlarmStatus::Idle);
    }

    #[tokio::test(start_paused = true)]
    async fn triggers_after_debounce() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        assert_eq!(alarm.check(true), AlarmStatus::Pending);

        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
    }

    #[tokio::test(start_paused = true)]
    async fn does_not_trigger_before_debounce() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        assert_eq!(alarm.check(true), AlarmStatus::Pending);

        time::advance(Duration::from_secs(29)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
    }

    #[tokio::test(start_paused = true)]
    async fn triggered_is_one_shot() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
        assert_eq!(alarm.check(true), AlarmStatus::Active);
        assert_eq!(alarm.check(true), AlarmStatus::Active);
    }

    #[tokio::test(start_paused = true)]
    async fn active_does_not_retrigger_after_another_window() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);

        // Even after another full debounce window, stays Active
        time::advance(Duration::from_secs(60)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Active);
    }

    #[tokio::test(start_paused = true)]
    async fn resolved_on_false_after_triggered() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(30)).await;
        alarm.check(true); // Triggered

        assert_eq!(alarm.check(false), AlarmStatus::Resolved);
    }

    #[tokio::test(start_paused = true)]
    async fn resolved_is_one_shot() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(30)).await;
        alarm.check(true); // Triggered

        assert_eq!(alarm.check(false), AlarmStatus::Resolved);
        assert_eq!(alarm.check(false), AlarmStatus::Idle);
    }

    #[tokio::test(start_paused = true)]
    async fn rearms_after_resolved() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));

        // First episode
        alarm.check(true);
        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
        assert_eq!(alarm.check(false), AlarmStatus::Resolved);

        // Second episode
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
    }

    #[tokio::test(start_paused = true)]
    async fn reset_from_timing_rearms() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(20)).await;
        alarm.reset();

        // Timer restarted -- need a full new window
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
        time::advance(Duration::from_secs(20)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
        time::advance(Duration::from_secs(10)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
    }

    #[tokio::test(start_paused = true)]
    async fn reset_from_fired_rearms() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.check(true);

        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);

        alarm.reset();

        // Back to idle -- a new episode can trigger
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
        time::advance(Duration::from_secs(30)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Triggered);
    }

    #[tokio::test(start_paused = true)]
    async fn reset_from_idle_is_noop() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));
        alarm.reset();
        assert_eq!(alarm.check(false), AlarmStatus::Idle);
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
    }

    #[tokio::test(start_paused = true)]
    async fn transient_true_does_not_accumulate() {
        let mut alarm = DebouncedAlarm::new(Duration::from_secs(30));

        // True for 20s, then false, then true for 20s -- should NOT
        // trigger because neither episode reached 30s.
        alarm.check(true);
        time::advance(Duration::from_secs(20)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Pending);

        alarm.check(false); // resets

        alarm.check(true);
        time::advance(Duration::from_secs(20)).await;
        assert_eq!(alarm.check(true), AlarmStatus::Pending);
    }
}
