//! Hold the LED in the off state.

use crate::tracing::prelude::*;
use tokio::sync::oneshot;

use super::{AnimationHandle, Resume};
use crate::hw_trait::rgb_led::RgbLed;

/// Turn the LED off and hold it there.
///
/// Like [`super::hold`], this provides a uniform [`AnimationHandle`]
/// for a static state. It calls [`RgbLed::off`] rather than setting
/// a color.
pub fn off(mut led: Box<dyn RgbLed>) -> AnimationHandle {
    // The task completes immediately after turning the LED off.
    let (cancel_tx, _cancel_rx) = oneshot::channel();
    let task = tokio::spawn(async move {
        if let Err(e) = led.off().await {
            warn!(error = %e, "LED off failed");
        }
        (
            led as Box<dyn RgbLed>,
            Box::new(OffState) as Box<dyn Resume>,
        )
    });
    AnimationHandle::new(cancel_tx, task)
}

struct OffState;

impl Resume for OffState {
    fn resume(self: Box<Self>, led: Box<dyn RgbLed>) -> AnimationHandle {
        off(led)
    }
}
