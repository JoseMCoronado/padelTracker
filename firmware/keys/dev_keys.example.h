// Copy to dev_keys.h (gitignored) and fill with your development keys.
// Never commit real keys (spec 10.8; see docs/PAIRING.md).
#pragma once

// 16-byte ESP-NOW Primary Master Key shared by the deployment.
#define PADEL_DEV_PMK { 'd', 'e', 'v', '-', 'p', 'm', 'k', '-', 'c', 'h', 'a', 'n', 'g', 'e', 'm', 'e' }

// 16-byte per-court Local Master Key (derive per docs/PAIRING.md).
#define PADEL_DEV_LMK_COURT1 { 'd', 'e', 'v', '-', 'l', 'm', 'k', '-', 'c', 'h', 'a', 'n', 'g', 'e', 'm', 'e' }
