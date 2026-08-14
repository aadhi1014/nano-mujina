//! 256-bit unsigned integer arithmetic.
//!
//! Wraps `ruint::aliases::U256` to provide a stable interface. This adapter
//! module exists so we can swap the underlying library or implement our own
//! arithmetic without changing callers.

use num_traits::Float;
use ruint::Uint;
use ruint::aliases::U256 as Ruint256;
use std::ops::{AddAssign, Div, Mul, Shl, SubAssign};

type U512 = Uint<512, 8>;

/// A 256-bit unsigned integer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct U256(Ruint256);

impl U256 {
    /// Zero constant.
    pub const ZERO: Self = Self(Ruint256::ZERO);

    /// Maximum value (2^256 - 1).
    pub const MAX: Self = Self(Ruint256::MAX);

    /// Create from little-endian bytes.
    pub fn from_le_bytes(bytes: [u8; 32]) -> Self {
        Self(Ruint256::from_le_bytes(bytes))
    }

    /// Convert to little-endian bytes.
    pub fn to_le_bytes(self) -> [u8; 32] {
        self.0.to_le_bytes()
    }

    /// Convert to u64, saturating at u64::MAX.
    pub fn saturating_to_u64(self) -> u64 {
        self.0.saturating_to()
    }

    /// Convert to f64, losing precision for large values.
    ///
    /// For values larger than f64 can precisely represent (~2^53), this
    /// returns an approximation by extracting the high bits and scaling.
    pub fn to_f64_approx(self) -> f64 {
        let bytes = self.to_le_bytes();

        // Find highest non-zero byte to determine magnitude
        let mut highest_byte = 0;
        for (i, &b) in bytes.iter().enumerate().rev() {
            if b != 0 {
                highest_byte = i;
                break;
            }
        }

        // If zero or fits in u64, use direct conversion
        if highest_byte < 8 {
            return self.saturating_to_u64() as f64;
        }

        // Extract 8 bytes starting from highest_byte-7 (or 0 if less)
        let start = highest_byte.saturating_sub(7);
        let mut buf = [0u8; 8];
        buf.copy_from_slice(&bytes[start..start + 8]);
        let mantissa = u64::from_le_bytes(buf) as f64;

        // Scale by 2^(start*8) to account for position
        mantissa * (2.0_f64).powi((start * 8) as i32)
    }

    /// Number of leading zero bits.
    fn leading_zeros(self) -> u32 {
        self.0.leading_zeros() as u32
    }

    /// Compute `(self << shift) / divisor` without overflow.
    ///
    /// Uses a 512-bit intermediate so callers don't need to worry
    /// about the left shift exceeding 256 bits. Saturates to
    /// [`U256::MAX`] if the final result doesn't fit in 256 bits.
    ///
    /// # Panics
    ///
    /// Panics if `divisor` is zero.
    fn shifted_div(self, shift: u32, divisor: u64) -> Self {
        // If the shifted value would exceed 512 bits, the result
        // certainly exceeds 256 bits, so saturate immediately.
        // (Zero is fine at any shift -- it stays zero.)
        if self != Self::ZERO {
            let self_bits = 256 - self.leading_zeros();
            if (self_bits as u64) + (shift as u64) > 512 {
                return Self::MAX;
            }
        }

        let limbs_256 = self.0.into_limbs();
        let mut limbs_512 = [0u64; 8];
        limbs_512[..4].copy_from_slice(&limbs_256);
        let wide = U512::from_limbs(limbs_512);

        let result = (wide << shift) / U512::from(divisor);

        let result_limbs = result.into_limbs();
        if result_limbs[4..].iter().any(|&l| l != 0) {
            return Self::MAX;
        }
        let mut narrow = [0u64; 4];
        narrow.copy_from_slice(&result_limbs[..4]);
        Self(Ruint256::from_limbs(narrow))
    }
}

impl Div for U256 {
    type Output = Self;

    fn div(self, rhs: Self) -> Self::Output {
        Self(self.0 / rhs.0)
    }
}

impl Div<u64> for U256 {
    type Output = Self;

    fn div(self, rhs: u64) -> Self::Output {
        Self(self.0 / Ruint256::from(rhs))
    }
}

impl Div<u128> for U256 {
    type Output = Self;

    fn div(self, rhs: u128) -> Self::Output {
        Self(self.0 / Ruint256::from(rhs))
    }
}

impl Div<f64> for U256 {
    type Output = Self;

    /// Divide by an f64, preserving all 53 bits of mantissa precision.
    ///
    /// Decomposes the float into its exact rational form (mantissa * 2^exp)
    /// and performs the division in integer arithmetic.
    ///
    /// # Panics
    ///
    /// Panics if `rhs` is zero, negative, NaN, or infinite.
    fn div(self, rhs: f64) -> Self::Output {
        assert!(
            rhs.is_finite() && rhs > 0.0,
            "f64 divisor must be finite and positive, got {rhs}"
        );

        let (mantissa, exponent, _sign) = rhs.integer_decode();
        let exponent = i32::from(exponent);

        if exponent >= 0 {
            let shift = exponent as u32;
            // Mantissa is at most 53 bits; if the shift pushes the
            // divisor beyond 256 bits the quotient is zero.
            if shift + 53 > 256 {
                return Self::ZERO;
            }
            let divisor = Self::from(mantissa) << shift;
            self / divisor
        } else {
            // self / (mantissa * 2^neg) = (self << -neg) / mantissa
            self.shifted_div((-exponent) as u32, mantissa)
        }
    }
}

impl Mul<u64> for U256 {
    type Output = Self;

    fn mul(self, rhs: u64) -> Self::Output {
        Self(self.0 * Ruint256::from(rhs))
    }
}

impl AddAssign for U256 {
    fn add_assign(&mut self, rhs: Self) {
        self.0 += rhs.0;
    }
}

impl SubAssign for U256 {
    fn sub_assign(&mut self, rhs: Self) {
        self.0 -= rhs.0;
    }
}

impl Shl<u32> for U256 {
    type Output = Self;

    fn shl(self, rhs: u32) -> Self::Output {
        Self(self.0 << rhs)
    }
}

impl From<u64> for U256 {
    fn from(value: u64) -> Self {
        Self(Ruint256::from(value))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_division_u256() {
        let a = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 100;
            bytes
        });
        let b = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 10;
            bytes
        });
        let expected = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 10;
            bytes
        });
        assert_eq!(a / b, expected);
    }

    #[test]
    fn test_division_u64() {
        let a = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 100;
            bytes
        });
        let expected = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 10;
            bytes
        });
        assert_eq!(a / 10u64, expected);
    }

    #[test]
    fn test_large_division() {
        // Large value / 1 = same value
        let large = U256::from_le_bytes([0xff; 32]);
        let one = U256::from_le_bytes({
            let mut bytes = [0u8; 32];
            bytes[0] = 1;
            bytes
        });
        assert_eq!(large / one, large);
    }

    #[test]
    fn test_from_u64() {
        let val = U256::from(42u64);
        assert_eq!(val.saturating_to_u64(), 42);

        let zero = U256::from(0u64);
        assert_eq!(zero, U256::ZERO);

        let max = U256::from(u64::MAX);
        assert_eq!(max.saturating_to_u64(), u64::MAX);
    }

    #[test]
    fn test_shl() {
        let one = U256::from(1u64);
        let two = U256::from(2u64);
        assert_eq!(one << 1, two);

        // Shift into upper half of the 256-bit range
        let high = U256::from(1u64) << 128;
        let mut expected = [0u8; 32];
        expected[16] = 1;
        assert_eq!(high, U256::from_le_bytes(expected));

        // Shift by zero is identity
        let val = U256::from(0xDEAD_u64);
        assert_eq!(val << 0, val);
    }

    #[test]
    fn test_shifted_div() {
        // (100 << 4) / 5 = 1600 / 5 = 320
        let val = U256::from(100u64);
        assert_eq!(val.shifted_div(4, 5).saturating_to_u64(), 320);

        // Shift that would overflow 256 bits but fits in 512
        let large = U256::MAX;
        let result = large.shifted_div(64, u64::MAX);
        assert!(result > U256::ZERO);

        // Result that overflows 256 bits saturates to MAX
        let result = U256::MAX.shifted_div(128, 1);
        assert_eq!(result, U256::MAX);

        // Shift exceeding 512-bit intermediate saturates instead of
        // wrapping to zero
        let result = U256::MAX.shifted_div(1074, 1);
        assert_eq!(result, U256::MAX);

        // Zero self with any shift is zero (not MAX)
        assert_eq!(U256::ZERO.shifted_div(1074, 1), U256::ZERO);
    }

    #[test]
    fn test_division_f64() {
        // Integer f64
        assert_eq!(U256::from(100u64) / 10.0_f64, U256::from(10u64));

        // Fractional divisor (< 1) uses shifted_div path
        assert_eq!((U256::from(100u64) / 0.5_f64).saturating_to_u64(), 200);
        assert_eq!((U256::from(1000u64) / 0.25_f64).saturating_to_u64(), 4000);

        // Enormous divisor: quotient is zero
        assert_eq!(U256::MAX / 1e100_f64, U256::ZERO);
        assert_eq!(U256::from(1u64) / f64::MAX, U256::ZERO);
    }

    #[test]
    #[should_panic(expected = "finite and positive")]
    fn test_division_f64_zero_panics() {
        let _ = U256::from(1u64) / 0.0_f64;
    }

    #[test]
    #[should_panic(expected = "finite and positive")]
    fn test_division_f64_nan_panics() {
        let _ = U256::from(1u64) / f64::NAN;
    }

    #[test]
    #[should_panic(expected = "finite and positive")]
    fn test_division_f64_negative_panics() {
        let _ = U256::from(1u64) / (-1.0_f64);
    }

    #[test]
    fn test_leading_zeros() {
        assert_eq!(U256::ZERO.leading_zeros(), 256);
        assert_eq!(U256::MAX.leading_zeros(), 0);
        assert_eq!(U256::from(1u64).leading_zeros(), 255);
        assert_eq!(U256::from(u64::MAX).leading_zeros(), 192);

        // Value with highest bit set
        let top_bit = U256::from(1u64) << 255;
        assert_eq!(top_bit.leading_zeros(), 0);
    }
}
