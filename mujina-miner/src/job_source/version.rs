//! Block version template with rolling capability.

use bitcoin::block::Version;

/// General purpose bits for version rolling (BIP320, bits 13-28).
///
/// A 2-byte bit pattern occupying positions 13-28 of the block
/// version field. Used as both a mask (which bits can roll in
/// VersionTemplate) and a value (which bits were actually rolled).
///
/// When applied to a base version, these bits are shifted left 13
/// positions and OR'd to produce the final block version.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GeneralPurposeBits([u8; 2]);

impl GeneralPurposeBits {
    pub fn new(bytes: [u8; 2]) -> Self {
        Self(bytes)
    }

    pub fn as_bytes(&self) -> &[u8; 2] {
        &self.0
    }

    /// Full mask - all 16 bits rollable
    pub fn full() -> Self {
        Self([0xff, 0xff])
    }

    /// No mask - no bits rollable
    pub fn none() -> Self {
        Self([0x00, 0x00])
    }

    /// Check if value bits fit within this mask
    pub fn contains(&self, value: &GeneralPurposeBits) -> bool {
        self.0
            .iter()
            .zip(value.0.iter())
            .all(|(m, b)| (b & !m) == 0)
    }

    /// Apply these bits to a base version to produce the final version.
    ///
    /// Shifts these bits left 13 positions and ORs with the base version.
    ///
    /// # Example
    ///
    /// ```
    /// use mujina_miner::job_source::GeneralPurposeBits;
    /// use bitcoin::block::Version;
    ///
    /// let base = Version::from_consensus(0x20000000);
    /// let gp_bits = GeneralPurposeBits::new([0x05, 0xa2]);
    /// let version = gp_bits.apply_to_version(base);
    /// assert_eq!(version.to_consensus(), 0x20b44000);
    /// ```
    pub fn apply_to_version(&self, base_version: Version) -> Version {
        let bits = u16::from_be_bytes(self.0);
        let base = base_version.to_consensus();
        let rolled = base | ((bits as i32) << 13);
        Version::from_consensus(rolled)
    }
}

impl From<[u8; 2]> for GeneralPurposeBits {
    fn from(bytes: [u8; 2]) -> Self {
        Self(bytes)
    }
}

impl From<GeneralPurposeBits> for [u8; 2] {
    fn from(gp: GeneralPurposeBits) -> Self {
        gp.0
    }
}

impl From<&[u8; 4]> for GeneralPurposeBits {
    /// Create from a 4-byte version mask (e.g., from Stratum mining.configure).
    ///
    /// Extracts bits 13-28 from the mask by interpreting the bytes as a big-endian
    /// u32, shifting right 13 positions, and taking the resulting 16 bits.
    ///
    /// # Example
    ///
    /// ```
    /// use mujina_miner::job_source::GeneralPurposeBits;
    ///
    /// // Stratum mask 0x1fffe000 (bits 13-28 all set)
    /// let mask_bytes = [0x1f, 0xff, 0xe0, 0x00];
    /// let gp_bits = GeneralPurposeBits::from(&mask_bytes);
    /// assert_eq!(gp_bits.as_bytes(), &[0xff, 0xff]);
    /// ```
    fn from(mask_bytes: &[u8; 4]) -> Self {
        let mask = u32::from_be_bytes(*mask_bytes);
        let bits = (mask >> 13) as u16;
        Self(bits.to_be_bytes())
    }
}

/// Errors from VersionTemplate operations
#[derive(Debug, thiserror::Error)]
pub enum VersionTemplateError {
    #[error(
        "Base version 0x{base_value:08x} has general purpose bits set in positions 13-28: {gp_bits:02x?}"
    )]
    BaseHasGeneralPurposeBits { base_value: u32, gp_bits: [u8; 2] },

    #[error("General purpose bits {gp_bits:02x?} exceed mask {mask:02x?}")]
    GpBitsExceedMask { gp_bits: [u8; 2], mask: [u8; 2] },
}

/// Template for block version with rolling capability.
///
/// Mining hardware can search additional nonce space by modifying bits in the
/// block version field, a technique called "version rolling" (BIP320). In Stratum v1,
/// the pool specifies which bits miners may modify via the `version_mask` parameter in
/// the `mining.configure` response.
#[derive(Debug, Clone)]
pub struct VersionTemplate {
    /// Base block version (must have bits 13-28 clear)
    base: Version,

    /// Mask indicating which GP bits (13-28) may be rolled
    gp_bits_mask: GeneralPurposeBits,
}

impl VersionTemplate {
    /// Create with validation that base doesn't conflict with mask.
    ///
    /// # Errors
    ///
    /// Returns `BaseHasGeneralPurposeBits` if the base version has any bits
    /// set in positions 13-28, since those bits are replaced by rolling.
    pub fn new(
        base: Version,
        gp_bits_mask: GeneralPurposeBits,
    ) -> Result<Self, VersionTemplateError> {
        // Extract bits 13-28 from base
        let base_u32 = base.to_consensus() as u32;
        let gp_region = (base_u32 >> 13) & 0xffff;

        if gp_region != 0 {
            return Err(VersionTemplateError::BaseHasGeneralPurposeBits {
                base_value: base_u32,
                gp_bits: [(gp_region >> 8) as u8, gp_region as u8],
            });
        }

        Ok(Self { base, gp_bits_mask })
    }

    pub fn base(&self) -> Version {
        self.base
    }

    pub fn gp_bits_mask(&self) -> GeneralPurposeBits {
        self.gp_bits_mask
    }

    /// Apply general purpose bits with validation.
    ///
    /// # Errors
    ///
    /// Returns `GpBitsExceedMask` if GP bits have any bits set outside the allowed mask.
    pub fn apply_gp_bits(
        &self,
        gp_bits: &GeneralPurposeBits,
    ) -> Result<Version, VersionTemplateError> {
        if !self.gp_bits_mask.contains(gp_bits) {
            return Err(VersionTemplateError::GpBitsExceedMask {
                gp_bits: gp_bits.0,
                mask: self.gp_bits_mask.0,
            });
        }

        Ok(gp_bits.apply_to_version(self.base))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version_template_rejects_conflicting_base() {
        // Base with bits in GP region (13-28) should be rejected
        let bad_base = Version::from_consensus(0x2e596000);
        let result = VersionTemplate::new(bad_base, GeneralPurposeBits::full());

        assert!(result.is_err());
        match result {
            Err(VersionTemplateError::BaseHasGeneralPurposeBits {
                base_value,
                gp_bits,
            }) => {
                assert_eq!(base_value, 0x2e596000);
                // Extract GP region: (0x2e596000 >> 13) & 0xffff = 0x72cb
                assert_eq!(gp_bits, [0x72, 0xcb]);
            }
            _ => panic!("Expected BaseHasGeneralPurposeBits error"),
        }
    }

    #[test]
    fn test_version_template_accepts_clean_base() {
        // Base with only BIP320 bit set (no GP bits) should be accepted
        let clean_base = Version::from_consensus(0x20000000);
        let result = VersionTemplate::new(clean_base, GeneralPurposeBits::full());

        assert!(result.is_ok());
        let template = result.unwrap();
        assert_eq!(template.base(), clean_base);
    }

    #[test]
    fn test_apply_gp_bits_validates_mask() {
        let template = VersionTemplate::new(
            Version::from_consensus(0x20000000),
            GeneralPurposeBits::new([0x0f, 0xff]), // Only some bits rollable
        )
        .unwrap();

        // GP bits that fit within mask should work
        let valid_bits = GeneralPurposeBits::new([0x00, 0xff]);
        assert!(template.apply_gp_bits(&valid_bits).is_ok());

        // GP bits that exceed mask should fail
        let invalid_bits = GeneralPurposeBits::new([0xff, 0xff]);
        let result = template.apply_gp_bits(&invalid_bits);

        assert!(result.is_err());
        match result {
            Err(VersionTemplateError::GpBitsExceedMask { gp_bits, mask }) => {
                assert_eq!(gp_bits, [0xff, 0xff]);
                assert_eq!(mask, [0x0f, 0xff]);
            }
            _ => panic!("Expected GpBitsExceedMask error"),
        }
    }

    #[test]
    fn test_gp_bits_contains() {
        let mask = GeneralPurposeBits::new([0x0f, 0xff]);

        // Bits within mask
        assert!(mask.contains(&GeneralPurposeBits::new([0x00, 0x00])));
        assert!(mask.contains(&GeneralPurposeBits::new([0x0f, 0xff])));
        assert!(mask.contains(&GeneralPurposeBits::new([0x00, 0xff])));
        assert!(mask.contains(&GeneralPurposeBits::new([0x05, 0xa2])));

        // Bits exceeding mask
        assert!(!mask.contains(&GeneralPurposeBits::new([0xff, 0xff])));
        assert!(!mask.contains(&GeneralPurposeBits::new([0x10, 0x00])));
        assert!(!mask.contains(&GeneralPurposeBits::new([0xf0, 0xff])));
    }

    #[test]
    fn test_gp_bits_full() {
        let full = GeneralPurposeBits::full();
        assert_eq!(full.as_bytes(), &[0xff, 0xff]);

        // Full mask should contain any bits
        assert!(full.contains(&GeneralPurposeBits::new([0x00, 0x00])));
        assert!(full.contains(&GeneralPurposeBits::new([0xff, 0xff])));
        assert!(full.contains(&GeneralPurposeBits::new([0xab, 0xcd])));
    }

    #[test]
    fn test_apply_to_version_bit_arithmetic() {
        // Zero base - GP bits alone
        let gp = GeneralPurposeBits::new([0x05, 0xa2]);
        let zero_base = Version::from_consensus(0);
        let result = gp.apply_to_version(zero_base);
        assert_eq!(result.to_consensus(), 0x00b4_4000);

        // Non-zero base - OR with GP bits
        let base = Version::from_consensus(0x20000000);
        let result = gp.apply_to_version(base);
        assert_eq!(result.to_consensus(), 0x20b4_4000);

        // Max GP bits
        let max_gp = GeneralPurposeBits::new([0xff, 0xff]);
        let result = max_gp.apply_to_version(base);
        assert_eq!(result.to_consensus(), 0x3fff_e000);

        // Zero GP bits - base unchanged
        let zero_gp = GeneralPurposeBits::new([0x00, 0x00]);
        let result = zero_gp.apply_to_version(base);
        assert_eq!(result.to_consensus(), 0x20000000);
    }

    #[test]
    fn test_apply_to_version_preserves_non_gp_bits() {
        // Version with bits set outside GP region (13-28)
        let base = Version::from_consensus(0xe000_1fffu32 as i32);
        let gp = GeneralPurposeBits::new([0xff, 0xff]);
        let result = gp.apply_to_version(base);

        // Original bits outside 13-28 should be preserved
        assert_eq!(
            result.to_consensus() & (0xe000_1fffu32 as i32),
            0xe000_1fffu32 as i32,
            "Bits outside GP region should be preserved"
        );

        // GP bits should be set
        let gp_region = (result.to_consensus() as u32 >> 13) & 0xffff;
        assert_eq!(gp_region, 0xffff);
    }
}
