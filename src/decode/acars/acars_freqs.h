// Standard VHF ACARS channel frequencies (MHz) on the 25 kHz raster.
// A superset of the commonly assigned channels; decoders on unused channels
// simply never lock. Reference: ARINC/ICAO ACARS VHF channel plan.
#pragma once

static const double kAcarsFreqsMHz[] = {
    129.125, 129.325, 129.400, 129.925,
    130.025, 130.425, 130.450, 130.625, 130.825,
    131.125, 131.175, 131.225, 131.275, 131.325, 131.375, 131.425, 131.475,
    131.525, 131.550, 131.625, 131.675, 131.725, 131.750, 131.775, 131.825,
    131.850, 131.875, 131.925, 131.950, 131.975,
    136.700, 136.725, 136.750, 136.775, 136.800, 136.825, 136.850, 136.875,
    136.900, 136.925, 136.950, 136.975,
};
static constexpr int kNumAcarsFreqs = (int)(sizeof(kAcarsFreqsMHz) / sizeof(kAcarsFreqsMHz[0]));

// The most commonly used ACARS channels — auto-populated by default.
static const double kAcarsCommonFreqsMHz[] = {
    131.550,  //  1: primary North America
    131.725,  //  2: primary Europe
    131.525,  //  3
    131.825,  //  4
    130.025,  //  5
};
static constexpr int kNumAcarsCommon =
    (int)(sizeof(kAcarsCommonFreqsMHz) / sizeof(kAcarsCommonFreqsMHz[0]));

