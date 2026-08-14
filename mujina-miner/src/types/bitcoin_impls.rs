//! Trait implementations bridging U256 and bitcoin types.

use bitcoin::hashes::Hash;
use bitcoin::pow::{Target, Work};

use super::BlockHash;
use crate::u256::U256;

impl From<Target> for U256 {
    fn from(target: Target) -> Self {
        Self::from_le_bytes(target.to_le_bytes())
    }
}

impl From<U256> for Target {
    fn from(u: U256) -> Self {
        Target::from_le_bytes(u.to_le_bytes())
    }
}

impl From<&BlockHash> for U256 {
    fn from(hash: &BlockHash) -> Self {
        Self::from_le_bytes(*hash.as_byte_array())
    }
}

impl From<Work> for U256 {
    fn from(work: Work) -> Self {
        Self::from_le_bytes(work.to_le_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_target_u256_roundtrip() {
        let target = Target::MAX;
        let u = U256::from(target);
        let back = Target::from(u);
        assert_eq!(target, back);
    }
}
