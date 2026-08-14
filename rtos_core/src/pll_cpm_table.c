/* A3197S PLL frequency (MHz) -> PLL-register-code lookup table.
 *
 * This array is generated, not transcribed -- see
 * rtos_core/tools/gen_pll_cpm_table.py (--generate). It is computed
 * entirely from a verified register bitfield model: every one of these
 * 293 32-bit codes is reconstructed, bit-for-bit, by
 *   REFDIV      = (reg >> 1) & 0x3F
 *   FBDIV       = (reg >> 7) & 0x1FF
 *   POSTDIV_EXP = (reg >> 17) & 0x7      (POSTDIV = 1 << POSTDIV_EXP)
 *   ENABLE      = reg & 1
 *   reg = 0x90000000 | (POSTDIV_EXP<<17) | (FBDIV<<7) | (REFDIV<<1) | ENABLE
 * with Fout = 24MHz * FBDIV / (2 * REFDIV * POSTDIV). This was checked
 * programmatically both ways -- decoding every code and recomputing
 * Fout, and re-encoding from the extracted fields back to the literal
 * 32-bit word -- with zero mismatches either direction, and a full
 * integer sweep of every possible query frequency (0-700MHz) against
 * get_cpm_pll()/get_cpm_freq()'s exact lookup semantics also shows zero
 * behavioral difference. Bits 20-31 are constant 0x900 in every row,
 * exactly as this layout predicts. 168 rows land on an integer Fout,
 * the other 125 have a true .5 fraction that this table's uint32_t
 * freq column silently truncates (consistent with get_cpm_freq()
 * masking `(pll & 0x9fffffff) | 1` before comparing -- it discards bit
 * 0, and ENABLE is 1 in every row here). Implied VCO
 * (24*FBDIV/(2*REFDIV)) stays within 954-1896 MHz across the whole
 * table, with POSTDIV stepping down each time the request crosses
 * ~120/238/477 MHz -- consistent with a conventional post-divider
 * keeping an internal VCO in range as the output frequency climbs.
 * FBDIV/REFDIV/POSTDIV are standard integer-N PLL terminology that
 * fits this data exactly (right bit widths, right positions, exact
 * reconstruction); they are this project's own label for a verified
 * arithmetic/bit-layout relationship, not a claim about anyone else's
 * internal documentation.
 *
 * One honest limit on how independent this is: the specific frequency
 * range and per-band resolution covered here (which combinations get
 * included, out of everything the VCO constraint alone would allow --
 * 948 valid combinations exist in that constraint alone, only 293 are
 * used here) is not derivable from the formula or the VCO band by
 * itself; it matches the range this specific chip's PLL config needs
 * to cover for this project, determined empirically. See
 * gen_pll_cpm_table.py's docstring for the exact disclosure.
 *
 * 56 of the 293 (frequency, code) pairs below are independently
 * confirmed by this project's own real captured wire traffic (full
 * capture.sr / long_full_capture.sr -- live PLL ramp episodes step
 * PLL0 through dozens of intermediate codes one-by-one on the real
 * wire; checked by grepping the decoded capture for every code written
 * to REG_PLL0-3_FREQ and cross-matching against this table). The other
 * 237 were never observed on the wire in any capture this project has;
 * their values here are computed by the formula above, not copied.
 */

#include "pll_cpm_table.h"

const uint32_t cpm_table[PLL_TABLE_ROWS][2] =
{
	{ 99,	0x90084203},
	{100,	0x90084303},	/* actual 100.5 MHz */
	{102,	0x90084403},
	{103,	0x90084503},	/* actual 103.5 MHz */
	{105,	0x90084603},
	{106,	0x90084703},	/* actual 106.5 MHz */
	{108,	0x90084803},
	{109,	0x90084903},	/* actual 109.5 MHz */
	{111,	0x90084a03},
	{112,	0x90084b03},	/* actual 112.5 MHz */
	{114,	0x90084c03},
	{115,	0x90084d03},	/* actual 115.5 MHz */
	{117,	0x90084e03},
	{118,	0x90084f03},	/* actual 118.5 MHz */
	{120,	0x90062803},
	{121,	0x90062883},	/* actual 121.5 MHz */
	{123,	0x90062903},
	{124,	0x90062983},	/* actual 124.5 MHz */
	{126,	0x90062a03},
	{127,	0x90062a83},	/* actual 127.5 MHz */
	{129,	0x90062b03},
	{130,	0x90062b83},	/* actual 130.5 MHz */
	{132,	0x90062c03},
	{133,	0x90062c83},	/* actual 133.5 MHz */
	{135,	0x90062d03},
	{136,	0x90062d83},	/* actual 136.5 MHz */
	{138,	0x90062e03},
	{139,	0x90062e83},	/* actual 139.5 MHz */
	{141,	0x90062f03},
	{142,	0x90062f83},	/* actual 142.5 MHz */
	{144,	0x90063003},
	{145,	0x90063083},	/* actual 145.5 MHz */
	{147,	0x90063103},
	{148,	0x90063183},	/* actual 148.5 MHz */
	{150,	0x90063203},
	{151,	0x90063283},	/* actual 151.5 MHz */
	{153,	0x90063303},
	{154,	0x90063383},	/* actual 154.5 MHz */
	{156,	0x90063403},
	{157,	0x90063483},	/* actual 157.5 MHz */
	{159,	0x90063503},
	{160,	0x90063583},	/* actual 160.5 MHz */
	{162,	0x90063603},
	{163,	0x90063683},	/* actual 163.5 MHz */
	{165,	0x90063703},
	{166,	0x90063783},	/* actual 166.5 MHz */
	{168,	0x90063803},
	{169,	0x90063883},	/* actual 169.5 MHz */
	{171,	0x90063903},
	{172,	0x90063983},	/* actual 172.5 MHz */
	{174,	0x90063a03},
	{175,	0x90063a83},	/* actual 175.5 MHz */
	{177,	0x90063b03},
	{178,	0x90063b83},	/* actual 178.5 MHz */
	{180,	0x90063c03},
	{181,	0x90063c83},	/* actual 181.5 MHz */
	{183,	0x90063d03},
	{184,	0x90063d83},	/* actual 184.5 MHz */
	{186,	0x90063e03},
	{187,	0x90063e83},	/* actual 187.5 MHz */
	{189,	0x90063f03},
	{190,	0x90063f83},	/* actual 190.5 MHz */
	{192,	0x90064003},
	{193,	0x90064083},	/* actual 193.5 MHz */
	{195,	0x90064103},
	{196,	0x90064183},	/* actual 196.5 MHz */
	{198,	0x90064203},
	{199,	0x90064283},	/* actual 199.5 MHz */
	{201,	0x90064303},
	{202,	0x90064383},	/* actual 202.5 MHz */
	{204,	0x90064403},
	{205,	0x90064483},	/* actual 205.5 MHz */
	{207,	0x90064503},
	{208,	0x90064583},	/* actual 208.5 MHz */
	{210,	0x90064603},
	{211,	0x90064683},	/* actual 211.5 MHz */
	{213,	0x90064703},
	{214,	0x90064783},	/* actual 214.5 MHz */
	{216,	0x90064803},
	{217,	0x90064883},	/* actual 217.5 MHz */
	{219,	0x90064903},
	{220,	0x90064983},	/* actual 220.5 MHz */
	{222,	0x90064a03},
	{223,	0x90064a83},	/* actual 223.5 MHz */
	{225,	0x90064b03},
	{226,	0x90064b83},	/* actual 226.5 MHz */
	{228,	0x90064c03},
	{229,	0x90064c83},	/* actual 229.5 MHz */
	{231,	0x90064d03},
	{232,	0x90064d83},	/* actual 232.5 MHz */
	{234,	0x90064e03},
	{235,	0x90064e83},	/* actual 235.5 MHz */
	{237,	0x90064f03},
	{238,	0x90044f85},	/* actual 238.5 MHz */
	{240,	0x90042803},
	{241,	0x90045085},	/* actual 241.5 MHz */
	{243,	0x90042883},
	{244,	0x90045185},	/* actual 244.5 MHz */
	{246,	0x90042903},
	{247,	0x90045285},	/* actual 247.5 MHz */
	{249,	0x90042983},
	{250,	0x90045385},	/* actual 250.5 MHz */
	{252,	0x90042a03},
	{253,	0x90045485},	/* actual 253.5 MHz */
	{255,	0x90042a83},
	{256,	0x90045585},	/* actual 256.5 MHz */
	{258,	0x90042b03},
	{259,	0x90045685},	/* actual 259.5 MHz */
	{261,	0x90042b83},
	{262,	0x90045785},	/* actual 262.5 MHz */
	{264,	0x90042c03},
	{265,	0x90045885},	/* actual 265.5 MHz */
	{267,	0x90042c83},
	{268,	0x90045985},	/* actual 268.5 MHz */
	{270,	0x90042d03},
	{271,	0x90045a85},	/* actual 271.5 MHz */
	{273,	0x90042d83},
	{274,	0x90045b85},	/* actual 274.5 MHz */
	{276,	0x90042e03},
	{277,	0x90045c85},	/* actual 277.5 MHz */
	{279,	0x90042e83},
	{280,	0x90045d85},	/* actual 280.5 MHz */
	{282,	0x90042f03},
	{283,	0x90045e85},	/* actual 283.5 MHz */
	{285,	0x90042f83},
	{286,	0x90045f85},	/* actual 286.5 MHz */
	{288,	0x90043003},
	{289,	0x90046085},	/* actual 289.5 MHz */
	{291,	0x90043083},
	{292,	0x90046185},	/* actual 292.5 MHz */
	{294,	0x90043103},
	{295,	0x90046285},	/* actual 295.5 MHz */
	{297,	0x90043183},
	{298,	0x90046385},	/* actual 298.5 MHz */
	{300,	0x90043203},
	{301,	0x90046485},	/* actual 301.5 MHz */
	{303,	0x90043283},
	{304,	0x90046585},	/* actual 304.5 MHz */
	{306,	0x90043303},
	{307,	0x90046685},	/* actual 307.5 MHz */
	{309,	0x90043383},
	{310,	0x90046785},	/* actual 310.5 MHz */
	{312,	0x90043403},
	{313,	0x90046885},	/* actual 313.5 MHz */
	{315,	0x90043483},
	{316,	0x90046985},	/* actual 316.5 MHz */
	{318,	0x90043503},
	{319,	0x90046a85},	/* actual 319.5 MHz */
	{321,	0x90043583},
	{322,	0x90046b85},	/* actual 322.5 MHz */
	{324,	0x90043603},
	{325,	0x90046c85},	/* actual 325.5 MHz */
	{327,	0x90043683},
	{328,	0x90046d85},	/* actual 328.5 MHz */
	{330,	0x90043703},
	{331,	0x90046e85},	/* actual 331.5 MHz */
	{333,	0x90043783},
	{334,	0x90046f85},	/* actual 334.5 MHz */
	{336,	0x90043803},
	{337,	0x90047085},	/* actual 337.5 MHz */
	{339,	0x90043883},
	{340,	0x90047185},	/* actual 340.5 MHz */
	{342,	0x90043903},
	{343,	0x90047285},	/* actual 343.5 MHz */
	{345,	0x90043983},
	{346,	0x90047385},	/* actual 346.5 MHz */
	{348,	0x90043a03},
	{349,	0x90047485},	/* actual 349.5 MHz */
	{351,	0x90043a83},
	{352,	0x90047585},	/* actual 352.5 MHz */
	{354,	0x90043b03},
	{355,	0x90047685},	/* actual 355.5 MHz */
	{357,	0x90043b83},
	{358,	0x90047785},	/* actual 358.5 MHz */
	{360,	0x90043c03},
	{361,	0x90047885},	/* actual 361.5 MHz */
	{363,	0x90043c83},
	{364,	0x90047985},	/* actual 364.5 MHz */
	{366,	0x90043d03},
	{367,	0x90047a85},	/* actual 367.5 MHz */
	{369,	0x90043d83},
	{370,	0x90047b85},	/* actual 370.5 MHz */
	{372,	0x90043e03},
	{373,	0x90047c85},	/* actual 373.5 MHz */
	{375,	0x90043e83},
	{376,	0x90047d85},	/* actual 376.5 MHz */
	{378,	0x90043f03},
	{379,	0x90047e85},	/* actual 379.5 MHz */
	{381,	0x90043f83},
	{382,	0x90047f85},	/* actual 382.5 MHz */
	{384,	0x90044003},
	{385,	0x90048085},	/* actual 385.5 MHz */
	{387,	0x90044083},
	{388,	0x90048185},	/* actual 388.5 MHz */
	{390,	0x90044103},
	{391,	0x90048285},	/* actual 391.5 MHz */
	{393,	0x90044183},
	{394,	0x90048385},	/* actual 394.5 MHz */
	{396,	0x90044203},
	{397,	0x90048485},	/* actual 397.5 MHz */
	{399,	0x90044283},
	{400,	0x90048585},	/* actual 400.5 MHz */
	{402,	0x90044303},
	{403,	0x90048685},	/* actual 403.5 MHz */
	{405,	0x90044383},
	{406,	0x90048785},	/* actual 406.5 MHz */
	{408,	0x90044403},
	{409,	0x90048885},	/* actual 409.5 MHz */
	{411,	0x90044483},
	{412,	0x90048985},	/* actual 412.5 MHz */
	{414,	0x90044503},
	{415,	0x90048a85},	/* actual 415.5 MHz */
	{417,	0x90044583},
	{418,	0x90048b85},	/* actual 418.5 MHz */
	{420,	0x90044603},
	{421,	0x90048c85},	/* actual 421.5 MHz */
	{423,	0x90044683},
	{424,	0x90048d85},	/* actual 424.5 MHz */
	{426,	0x90044703},
	{427,	0x90048e85},	/* actual 427.5 MHz */
	{429,	0x90044783},
	{430,	0x90048f85},	/* actual 430.5 MHz */
	{432,	0x90044803},
	{433,	0x90049085},	/* actual 433.5 MHz */
	{435,	0x90044883},
	{436,	0x90049185},	/* actual 436.5 MHz */
	{438,	0x90044903},
	{439,	0x90049285},	/* actual 439.5 MHz */
	{441,	0x90044983},
	{442,	0x90049385},	/* actual 442.5 MHz */
	{444,	0x90044a03},
	{445,	0x90049485},	/* actual 445.5 MHz */
	{447,	0x90044a83},
	{448,	0x90049585},	/* actual 448.5 MHz */
	{450,	0x90044b03},
	{451,	0x90049685},	/* actual 451.5 MHz */
	{453,	0x90044b83},
	{454,	0x90049785},	/* actual 454.5 MHz */
	{456,	0x90044c03},
	{457,	0x90049885},	/* actual 457.5 MHz */
	{459,	0x90044c83},
	{460,	0x90049985},	/* actual 460.5 MHz */
	{462,	0x90044d03},
	{463,	0x90049a85},	/* actual 463.5 MHz */
	{465,	0x90044d83},
	{466,	0x90049b85},	/* actual 466.5 MHz */
	{468,	0x90044e03},
	{469,	0x90049c85},	/* actual 469.5 MHz */
	{471,	0x90044e83},
	{472,	0x90049d85},	/* actual 472.5 MHz */
	{474,	0x90044f03},
	{477,	0x90024f85},
	{480,	0x90022803},
	{483,	0x90025085},
	{486,	0x90022883},
	{489,	0x90025185},
	{492,	0x90022903},
	{495,	0x90025285},
	{498,	0x90022983},
	{501,	0x90025385},
	{504,	0x90022a03},
	{507,	0x90025485},
	{510,	0x90022a83},
	{513,	0x90025585},
	{516,	0x90022b03},
	{519,	0x90025685},
	{522,	0x90022b83},
	{525,	0x90025785},
	{528,	0x90022c03},
	{531,	0x90025885},
	{534,	0x90022c83},
	{537,	0x90025985},
	{540,	0x90022d03},
	{543,	0x90025a85},
	{546,	0x90022d83},
	{549,	0x90025b85},
	{552,	0x90022e03},
	{555,	0x90025c85},
	{558,	0x90022e83},
	{561,	0x90025d85},
	{564,	0x90022f03},
	{567,	0x90025e85},
	{570,	0x90022f83},
	{573,	0x90025f85},
	{576,	0x90023003},
	{579,	0x90026085},
	{582,	0x90023083},
	{585,	0x90026185},
	{588,	0x90023103},
	{591,	0x90026285},
	{594,	0x90023183},
	{597,	0x90026385},
	{600,	0x90023203},
};

/* =========================================================================
 * WARNING -- UNVALIDATED ABOVE 600MHz. DO NOT USE ON REAL HARDWARE.
 * =========================================================================
 * The 33 rows below (600-699MHz) are NOT part of cpm_table above and are
 * NOT used by get_cpm_pll()/get_cpm_freq() or anywhere else in this
 * codebase -- deliberately. They exist only as a documented reference for
 * what the verified PLL formula (see cpm_table's header comment above)
 * predicts in this range, generated by rtos_core/tools/gen_pll_cpm_table.py
 * --extend-700, while staying inside the measured VCO band (954-1896MHz).
 *
 * This range has never been run on real silicon. The original table
 * this project's cpm_table is based on -- and every real capture this
 * project has -- stops at 600MHz; the highest frequency ever actually
 * commanded on this hardware is
 * ~480MHz (stock HIGH mode). The arithmetic being self-consistent proves
 * the register encoding is well-formed, NOT that the chip can lock,
 * survive thermally, or produce correct output at these frequencies.
 * Wiring this into get_cpm_pll()/get_cpm_freq() (e.g. by renaming it to
 * cpm_table, or appending it there) would make these codes selectable by
 * anything that requests a frequency >=600MHz -- autotune, a manual
 * override, a bug -- with no interlock at all, since get_cpm_pll() is an
 * unguarded linear scan. Do not do that without a deliberate, incremental,
 * live-monitored (temp/voltage/nonce-error) hardware validation pass
 * first, the same way every other frequency change in this project was
 * validated -- see feedback_dont_overclock_too_much in project memory.
 */
static const uint32_t cpm_table_ABOVE_600MHZ_UNVALIDATED[33][2] __attribute__((unused)) =
{
	{603,	0x90026485},
	{606,	0x90023283},
	{609,	0x90026585},
	{612,	0x90023303},
	{615,	0x90026685},
	{618,	0x90023383},
	{621,	0x90026785},
	{624,	0x90023403},
	{627,	0x90026885},
	{630,	0x90023483},
	{633,	0x90026985},
	{636,	0x90023503},
	{639,	0x90026a85},
	{642,	0x90023583},
	{645,	0x90026b85},
	{648,	0x90023603},
	{651,	0x90026c85},
	{654,	0x90023683},
	{657,	0x90026d85},
	{660,	0x90023703},
	{663,	0x90026e85},
	{666,	0x90023783},
	{669,	0x90026f85},
	{672,	0x90023803},
	{675,	0x90027085},
	{678,	0x90023883},
	{681,	0x90027185},
	{684,	0x90023903},
	{687,	0x90027285},
	{690,	0x90023983},
	{693,	0x90027385},
	{696,	0x90023a03},
	{699,	0x90027485},
};

/* =========================================================================
 * WARNING -- UNVALIDATED BELOW 99MHz. DO NOT USE ON REAL HARDWARE.
 * =========================================================================
 * The 102 rows below (50-98MHz) are NOT part of cpm_table above and are
 * NOT used by get_cpm_pll()/get_cpm_freq() -- deliberately, same reasoning
 * as the >=600MHz block above. Two different confidence levels are mixed
 * in here and that distinction matters:
 *
 *  - 60-98MHz (52 rows) extends POSTDIV_EXP=4 (D=16) below the existing
 *    table's own floor (which starts at 99MHz using this exact same
 *    divider value) down to the VCO band's lower edge. Same divider
 *    value the real table already uses, just an unused portion of its
 *    FBDIV range -- same category of extrapolation as the >=600MHz block.
 *
 *  - 50-59MHz (50 rows) requires POSTDIV_EXP=5 (D=32), a divider value
 *    that appears NOWHERE else -- not in cpm_table, not in the >=600MHz
 *    block, not in any real captured wire traffic this project has. This
 *    is a genuinely new, never-observed field value, not just an unused
 *    range of a known one. Nothing about the arithmetic being
 *    self-consistent says this divider setting is physically valid on
 *    this silicon.
 *
 * Nothing in this project has ever run this chip below ~99MHz (its own
 * PLL bring-up default). As with the high-frequency block: do not wire
 * this into get_cpm_pll()/get_cpm_freq() without a deliberate,
 * incremental, live-monitored hardware validation pass first.
 */
static const uint32_t cpm_table_BELOW_99MHZ_UNVALIDATED[102][2] __attribute__((unused)) =
{
	{50,	0x900a8585},	/* actual 50.0625 MHz */
	{50,	0x900a4303},	/* actual 50.25 MHz */
	{50,	0x900a8685},	/* actual 50.4375 MHz */
	{50,	0x900a4383},	/* actual 50.625 MHz */
	{50,	0x900a8785},	/* actual 50.8125 MHz */
	{51,	0x900a4403},
	{51,	0x900a8885},	/* actual 51.1875 MHz */
	{51,	0x900a4483},	/* actual 51.375 MHz */
	{51,	0x900a8985},	/* actual 51.5625 MHz */
	{51,	0x900a4503},	/* actual 51.75 MHz */
	{51,	0x900a8a85},	/* actual 51.9375 MHz */
	{52,	0x900a4583},	/* actual 52.125 MHz */
	{52,	0x900a8b85},	/* actual 52.3125 MHz */
	{52,	0x900a4603},	/* actual 52.5 MHz */
	{52,	0x900a8c85},	/* actual 52.6875 MHz */
	{52,	0x900a4683},	/* actual 52.875 MHz */
	{53,	0x900a8d85},	/* actual 53.0625 MHz */
	{53,	0x900a4703},	/* actual 53.25 MHz */
	{53,	0x900a8e85},	/* actual 53.4375 MHz */
	{53,	0x900a4783},	/* actual 53.625 MHz */
	{53,	0x900a8f85},	/* actual 53.8125 MHz */
	{54,	0x900a4803},
	{54,	0x900a9085},	/* actual 54.1875 MHz */
	{54,	0x900a4883},	/* actual 54.375 MHz */
	{54,	0x900a9185},	/* actual 54.5625 MHz */
	{54,	0x900a4903},	/* actual 54.75 MHz */
	{54,	0x900a9285},	/* actual 54.9375 MHz */
	{55,	0x900a4983},	/* actual 55.125 MHz */
	{55,	0x900a9385},	/* actual 55.3125 MHz */
	{55,	0x900a4a03},	/* actual 55.5 MHz */
	{55,	0x900a9485},	/* actual 55.6875 MHz */
	{55,	0x900a4a83},	/* actual 55.875 MHz */
	{56,	0x900a9585},	/* actual 56.0625 MHz */
	{56,	0x900a4b03},	/* actual 56.25 MHz */
	{56,	0x900a9685},	/* actual 56.4375 MHz */
	{56,	0x900a4b83},	/* actual 56.625 MHz */
	{56,	0x900a9785},	/* actual 56.8125 MHz */
	{57,	0x900a4c03},
	{57,	0x900a9885},	/* actual 57.1875 MHz */
	{57,	0x900a4c83},	/* actual 57.375 MHz */
	{57,	0x900a9985},	/* actual 57.5625 MHz */
	{57,	0x900a4d03},	/* actual 57.75 MHz */
	{57,	0x900a9a85},	/* actual 57.9375 MHz */
	{58,	0x900a4d83},	/* actual 58.125 MHz */
	{58,	0x900a9b85},	/* actual 58.3125 MHz */
	{58,	0x900a4e03},	/* actual 58.5 MHz */
	{58,	0x900a9c85},	/* actual 58.6875 MHz */
	{58,	0x900a4e83},	/* actual 58.875 MHz */
	{59,	0x900a9d85},	/* actual 59.0625 MHz */
	{59,	0x900a4f03},	/* actual 59.25 MHz */
	{60,	0x90082803},
	{60,	0x90082883},	/* actual 60.75 MHz */
	{61,	0x90082903},	/* actual 61.5 MHz */
	{62,	0x90082983},	/* actual 62.25 MHz */
	{63,	0x90082a03},
	{63,	0x90082a83},	/* actual 63.75 MHz */
	{64,	0x90082b03},	/* actual 64.5 MHz */
	{65,	0x90082b83},	/* actual 65.25 MHz */
	{66,	0x90082c03},
	{66,	0x90082c83},	/* actual 66.75 MHz */
	{67,	0x90082d03},	/* actual 67.5 MHz */
	{68,	0x90082d83},	/* actual 68.25 MHz */
	{69,	0x90082e03},
	{69,	0x90082e83},	/* actual 69.75 MHz */
	{70,	0x90082f03},	/* actual 70.5 MHz */
	{71,	0x90082f83},	/* actual 71.25 MHz */
	{72,	0x90083003},
	{72,	0x90083083},	/* actual 72.75 MHz */
	{73,	0x90083103},	/* actual 73.5 MHz */
	{74,	0x90083183},	/* actual 74.25 MHz */
	{75,	0x90083203},
	{75,	0x90083283},	/* actual 75.75 MHz */
	{76,	0x90083303},	/* actual 76.5 MHz */
	{77,	0x90083383},	/* actual 77.25 MHz */
	{78,	0x90083403},
	{78,	0x90083483},	/* actual 78.75 MHz */
	{79,	0x90083503},	/* actual 79.5 MHz */
	{80,	0x90083583},	/* actual 80.25 MHz */
	{81,	0x90083603},
	{81,	0x90083683},	/* actual 81.75 MHz */
	{82,	0x90083703},	/* actual 82.5 MHz */
	{83,	0x90083783},	/* actual 83.25 MHz */
	{84,	0x90083803},
	{84,	0x90083883},	/* actual 84.75 MHz */
	{85,	0x90083903},	/* actual 85.5 MHz */
	{86,	0x90083983},	/* actual 86.25 MHz */
	{87,	0x90083a03},
	{87,	0x90083a83},	/* actual 87.75 MHz */
	{88,	0x90083b03},	/* actual 88.5 MHz */
	{89,	0x90083b83},	/* actual 89.25 MHz */
	{90,	0x90083c03},
	{90,	0x90083c83},	/* actual 90.75 MHz */
	{91,	0x90083d03},	/* actual 91.5 MHz */
	{92,	0x90083d83},	/* actual 92.25 MHz */
	{93,	0x90083e03},
	{93,	0x90083e83},	/* actual 93.75 MHz */
	{94,	0x90083f03},	/* actual 94.5 MHz */
	{95,	0x90083f83},	/* actual 95.25 MHz */
	{96,	0x90084003},
	{96,	0x90084083},	/* actual 96.75 MHz */
	{97,	0x90084103},	/* actual 97.5 MHz */
	{98,	0x90084183},	/* actual 98.25 MHz */
};
