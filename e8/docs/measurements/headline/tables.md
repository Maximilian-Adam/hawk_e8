# HAWK vs E8 comparison, run set headline: 10 run(s): 20261101T180503Z-3868e103 (O2), 20261101T181241Z-03d6bc94 (O3native), 20261101T181845Z-aebb1bea (O2), 20261101T182623Z-b256e741 (O3native), 20261101T183228Z-d046fc2d (O2), 20261101T184006Z-0fb0bd3e (O3native), 20261101T184610Z-905cc769 (O2), 20261101T185348Z-a9faef6f (O3native), 20261101T185953Z-82d98c58 (O2), 20261101T190731Z-9bdbc684 (O3native)

- 20261101T180503Z-3868e103: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T181241Z-03d6bc94: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T181845Z-aebb1bea: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T182623Z-b256e741: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T183228Z-d046fc2d: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T184006Z-0fb0bd3e: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T184610Z-905cc769: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T185348Z-a9faef6f: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T185953Z-82d98c58: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3
- 20261101T190731Z-9bdbc684: AMD Ryzen 9 9950X3D 16-Core Processor; Linux 7.1.5-arch1-1 #1 SMP PREEMPT_DYNAMIC Sun, 26 Jul 2026 00:49:06 +0000 x86_64; boost off; governor cpu0=performance cpu1=performance cpu2=performance cpu3=performance cpu4=performance cpu5=performance cpu6=performance cpu7=performance; SMT off; cpus 0,1,2,3,4,5,6,7; core cycles available; git 35e714ccd3

Acceptance:

    verify             PASS 49200000 signatures (and every warm-up) signed and verified, 0 failures
    complete           PASS 10 run(s), 100 CSV parts: every configuration with both accounting modes, equal counts, warm-up, keygen and prepare rows
    attempts           PASS 396 keys: every run and process signed the same signatures (identical attempts and rng_bytes), attempts per signature within 5 sigma of the validation's (HAWK n = 256 about 1.1796, the rest about 1.0000)
    run_spread         PASS 444 keys with >= 2 runs: per-run medians within 3.0%
    core_vs_tsc        PASS serial core/TSC ratios within 2.0% in each of 10 runs
    joins              PASS every key has its precision_summary and validation_summary row; threshold_bits agree
    preflight          PASS every run passed the preflight (boost off, performance governor, SMT off, own cores, perf, equal frequencies, clean tree)
    keyverify_complete PASS 20 keyverify CSV(s): every row count equals the --list schedule
    verify_failures    PASS 2400000 timed verifications accepted; every keygen, keygen_core, pub_decode and warm-up row ok
    verify_reject      PASS 6000 tampered signatures, every one rejected by every verifier in both modes

## Table 1: Headline, one core, as-specified precision: HAWK ref vs E8 ref (flags O2, run set headline)

cycles: core cycles (primary); TSC and us secondary. Estimates: median per run, mean over runs. all-in: from the encoded private key; prepared: from a prepared key (HAWK decoded, E8 prepared basis; preparation timed separately). attempts, sig B (encoded signature bytes: HAWK compressed, E8 uncompressed) and rng B (random bytes) per signature. Precision: threshold bits, per-signature SD bound (log2), RD loss factor at the optimal order and eq. (6) of the HAWK spec (e8/docs/precision_summary.csv).

| n | acct | scheme | variant | cycles | TSC | us | attempts | sig B | rng B | bits | SD log2 | loss | eq6 | E8/HAWK [95% CI] |
| ---: | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 256 | all-in | HAWK | ref | 122,975 | 122,344 | 28.5 | 1.1853 | 249 | 6,133 | 78 | -67.594 | 1.0001 | yes |  |
| 256 | all-in | E8 | ref | 307,484 | 306,495 | 71.5 | 1.0000 | 1,038 | 5,814 | 78 | -67.057 | 1.0000 | yes | 2.500 [2.498, 2.503] |
| 256 | prepared | HAWK | ref | 126,837 | 126,179 | 29.4 | 1.1822 | 249 | 6,117 | 78 | -67.594 | 1.0001 | yes |  |
| 256 | prepared | E8 | ref | 253,226 | 252,178 | 58.8 | 1.0000 | 1,038 | 5,814 | 78 | -67.057 | 1.0000 | yes | 1.996 [1.995, 1.998] |
| 512 | all-in | HAWK | ref | 257,459 | 256,400 | 59.8 | 1.0000 | 555 | 10,304 | 78 | -66.836 | 1.0602 | yes |  |
| 512 | all-in | E8 | ref | 626,078 | 623,990 | 145.4 | 1.0000 | 2,072 | 11,584 | 78 | -66.157 | 1.0290 | yes | 2.432 [2.431, 2.433] |
| 512 | prepared | HAWK | ref | 262,320 | 261,242 | 60.9 | 1.0000 | 555 | 10,304 | 78 | -66.836 | 1.0602 | yes |  |
| 512 | prepared | E8 | ref | 511,313 | 509,507 | 118.8 | 1.0000 | 2,072 | 11,584 | 78 | -66.157 | 1.0290 | yes | 1.949 [1.948, 1.950] |
| 1024 | all-in | HAWK | ref | 565,264 | 563,352 | 131.3 | 1.0000 | 1,221 | 20,560 | 78 | -65.229 | 1.1674 | yes |  |
| 1024 | all-in | E8 | ref | 1,319,246 | 1,316,054 | 306.7 | 1.0000 | 4,136 | 23,120 | 78 | -65.256 | 1.1251 | yes | 2.334 [2.323, 2.341] |
| 1024 | prepared | HAWK | ref | 539,112 | 537,276 | 125.2 | 1.0000 | 1,221 | 20,560 | 78 | -65.229 | 1.1674 | yes |  |
| 1024 | prepared | E8 | ref | 1,051,119 | 1,048,374 | 244.3 | 1.0000 | 4,136 | 23,120 | 78 | -65.256 | 1.1251 | yes | 1.950 [1.940, 1.957] |

## Table 2: Same technique, one core: hot64 and hot32 (flags O2, run set headline)

cycles: core cycles (primary); TSC and us secondary. Estimates: median per run, mean over runs. all-in: from the encoded private key; prepared: from a prepared key (HAWK decoded, E8 prepared basis; preparation timed separately). attempts, sig B (encoded signature bytes: HAWK compressed, E8 uncompressed) and rng B (random bytes) per signature. Precision: threshold bits, per-signature SD bound (log2), RD loss factor at the optimal order and eq. (6) of the HAWK spec (e8/docs/precision_summary.csv).

| n | acct | scheme | variant | cycles | TSC | us | attempts | sig B | rng B | bits | SD log2 | loss | eq6 | E8/HAWK [95% CI] |
| ---: | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 256 | all-in | HAWK | hot64 | 123,805 | 123,152 | 28.7 | 1.1762 | 249 | 4,898 | 64 | -54.585 | 1.0002 | yes |  |
| 256 | all-in | E8 | hot64 | 198,828 | 198,015 | 46.2 | 1.0000 | 1,038 | 4,662 | 64 | -54.130 | 1.0000 | yes | 1.606 [1.604, 1.608] |
| 256 | prepared | HAWK | hot64 | 126,766 | 126,085 | 29.4 | 1.1785 | 249 | 4,908 | 64 | -54.585 | 1.0002 | yes |  |
| 256 | prepared | E8 | hot64 | 144,676 | 143,930 | 33.6 | 1.0000 | 1,038 | 4,662 | 64 | -54.130 | 1.0000 | yes | 1.141 [1.140, 1.142] |
| 512 | all-in | HAWK | hot64 | 258,223 | 257,174 | 60.0 | 1.0000 | 555 | 8,423 | 64 | -53.386 | 1.8e+09 | no |  |
| 512 | all-in | E8 | hot64 | 409,975 | 408,500 | 95.2 | 1.0000 | 2,072 | 9,280 | 64 | -53.040 | 1.0036 | yes | 1.588 [1.587, 1.589] |
| 512 | prepared | HAWK | hot64 | 262,262 | 261,199 | 60.9 | 1.0000 | 555 | 8,423 | 64 | -53.386 | 1.8e+09 | no |  |
| 512 | prepared | E8 | hot64 | 295,110 | 293,931 | 68.5 | 1.0000 | 2,072 | 9,280 | 64 | -53.040 | 1.0036 | yes | 1.125 [1.125, 1.126] |
| 1024 | all-in | HAWK | hot64 | 559,592 | 557,753 | 130.0 | 1.0000 | 1,221 | 16,832 | 64 | -52.858 | 1.9e+15 | no |  |
| 1024 | all-in | E8 | hot64 | 886,659 | 884,218 | 206.1 | 1.0000 | 4,136 | 18,512 | 64 | -51.732 | 1.0061 | yes | 1.584 [1.584, 1.585] |
| 1024 | prepared | HAWK | hot64 | 537,662 | 535,875 | 124.9 | 1.0000 | 1,221 | 16,832 | 64 | -52.858 | 1.9e+15 | no |  |
| 1024 | prepared | E8 | hot64 | 618,420 | 616,560 | 143.7 | 1.0000 | 4,136 | 18,512 | 64 | -51.732 | 1.0061 | yes | 1.150 [1.150, 1.150] |
| 256 | all-in | HAWK | hot32 | 101,384 | 100,792 | 23.5 | 1.1753 | 249 | 2,479 | 32 | -22.841 | 7.8980 | no |  |
| 256 | all-in | E8 | hot32 | 174,136 | 173,350 | 40.4 | 1.0000 | 1,038 | 2,358 | 32 | -21.886 | 16.8477 | no | 1.718 [1.715, 1.722] |
| 256 | prepared | HAWK | hot32 | 104,354 | 103,750 | 24.2 | 1.1702 | 249 | 2,468 | 32 | -22.841 | 7.8980 | no |  |
| 256 | prepared | E8 | hot32 | 120,270 | 119,600 | 27.9 | 1.0000 | 1,038 | 2,358 | 32 | -21.886 | 16.8477 | no | 1.153 [1.151, 1.156] |
| 512 | all-in | HAWK | hot32 | 213,236 | 212,300 | 49.5 | 1.0000 | 555 | 4,244 | 32 | -21.402 | inf | no |  |
| 512 | all-in | E8 | hot32 | 361,092 | 359,729 | 83.9 | 1.0000 | 2,072 | 4,672 | 32 | -20.726 | inf | no | 1.693 [1.693, 1.694] |
| 512 | prepared | HAWK | hot32 | 217,297 | 216,333 | 50.4 | 1.0000 | 555 | 4,244 | 32 | -21.402 | inf | no |  |
| 512 | prepared | E8 | hot32 | 246,190 | 245,160 | 57.2 | 1.0000 | 2,072 | 4,672 | 32 | -20.726 | inf | no | 1.133 [1.132, 1.134] |
| 1024 | all-in | HAWK | hot32 | 470,533 | 468,906 | 109.3 | 1.0000 | 1,221 | 8,456 | 32 | -20.221 | inf | no |  |
| 1024 | all-in | E8 | hot32 | 789,012 | 786,702 | 183.3 | 1.0000 | 4,136 | 9,296 | 32 | -19.959 | inf | no | 1.677 [1.676, 1.677] |
| 1024 | prepared | HAWK | hot32 | 448,506 | 446,933 | 104.2 | 1.0000 | 1,221 | 8,456 | 32 | -20.221 | inf | no |  |
| 1024 | prepared | E8 | hot32 | 520,861 | 519,195 | 121.0 | 1.0000 | 4,136 | 9,296 | 32 | -19.959 | inf | no | 1.161 [1.161, 1.162] |

## Table 3: Sampler only, one thread: cycles and random bytes per 8 coordinates (flags O2, run set headline)

e8_sampler_bench --variant-matrix: median per call over its trials, divided by n/4 (2n coordinates per call); bytes include the 40-byte seed; mean over runs.

| scheme | variant | bits | technique | n | cycles / 8 | TSC / 8 | bytes / 8 | runs |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| HAWK | ref | 78 | ct full scan | 256 | 992.5 | 983.4 | 80.6 | 5 |
| HAWK | ref-T | 78 | ct full scan | 256 | 1030.5 | 1021.5 | 80.6 | 5 |
| HAWK | hot64 | 64 | vt hot table | 256 | 1012.9 | 1004.2 | 64.8 | 5 |
| HAWK | hot32 | 32 | vt hot table | 256 | 659.0 | 651.6 | 32.7 | 5 |
| E8 | ref | 78 | ct full scan | 256 | 2986.4 | 2969.4 | 90.6 | 5 |
| E8 | hot64 | 64 | vt hot table | 256 | 1226.4 | 1215.9 | 72.6 | 5 |
| E8 | hot32 | 32 | vt hot table | 256 | 841.2 | 832.6 | 36.6 | 5 |
| HAWK | ref | 78 | ct full scan | 512 | 1003.0 | 996.3 | 80.3 | 5 |
| HAWK | ref-T | 78 | ct full scan | 512 | 1072.0 | 1065.1 | 80.3 | 5 |
| HAWK | hot64 | 64 | vt hot table | 512 | 1015.1 | 1008.6 | 65.6 | 5 |
| HAWK | hot32 | 32 | vt hot table | 512 | 659.8 | 654.8 | 33.0 | 5 |
| E8 | ref | 78 | ct full scan | 512 | 2957.3 | 2943.1 | 90.3 | 5 |
| E8 | hot64 | 64 | vt hot table | 512 | 1209.0 | 1201.4 | 72.3 | 5 |
| E8 | hot32 | 32 | vt hot table | 512 | 822.0 | 816.2 | 36.3 | 5 |
| HAWK | ref | 78 | ct full scan | 1024 | 1017.3 | 1011.6 | 80.2 | 5 |
| HAWK | ref-T | 78 | ct full scan | 1024 | 1060.4 | 1054.8 | 80.2 | 5 |
| HAWK | hot64 | 64 | vt hot table | 1024 | 999.1 | 993.7 | 65.6 | 5 |
| HAWK | hot32 | 32 | vt hot table | 1024 | 650.1 | 646.1 | 32.9 | 5 |
| E8 | ref | 78 | ct full scan | 1024 | 2952.9 | 2940.0 | 90.2 | 5 |
| E8 | hot64 | 64 | vt hot table | 1024 | 1201.7 | 1195.7 | 72.2 | 5 |
| E8 | hot32 | 32 | vt hot table | 1024 | 817.1 | 812.5 | 36.2 | 5 |

## Table 4: Parallel latency: wall-clock and total CPU cycles per signature (flags O2, run set headline)

TSC: wall-clock cycles of the signing call; core: core cycles summed over the caller and the pool workers' jobs (total CPU cycles; empty if the counter is unavailable). HAWK ref-T is HAWK's split ref on the same bound worker streams as E8.

| pair | n | acct | T | HAWK TSC | HAWK core | E8 TSC | E8 core | E8/HAWK TSC [95% CI] | E8/HAWK core |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| ref-T / ref | 256 | all-in | 1 | 124,373 | 125,023 | 306,495 | 307,484 | 2.464 [2.462, 2.468] | 2.459 |
| ref-T / ref | 256 | all-in | 2 | 95,357 | 129,579 | 217,683 | 313,870 | 2.283 [2.280, 2.285] | 2.422 |
| ref-T / ref | 256 | all-in | 4 | 80,702 | 135,733 | 173,290 | 323,561 | 2.147 [2.145, 2.150] | 2.384 |
| ref-T / ref | 256 | all-in | 8 | 73,771 | 143,405 | 151,093 | 333,400 | 2.048 [2.046, 2.051] | 2.325 |
| ref-T / ref | 256 | prepared | 1 | 127,306 | 127,992 | 252,178 | 253,226 | 1.981 [1.979, 1.982] | 1.978 |
| ref-T / ref | 256 | prepared | 2 | 98,272 | 131,571 | 164,217 | 259,092 | 1.671 [1.671, 1.671] | 1.969 |
| ref-T / ref | 256 | prepared | 4 | 83,601 | 135,795 | 119,987 | 266,962 | 1.435 [1.434, 1.437] | 1.966 |
| ref-T / ref | 256 | prepared | 8 | 76,652 | 139,629 | 97,636 | 272,724 | 1.274 [1.273, 1.275] | 1.953 |
| ref-T / ref | 512 | all-in | 1 | 264,347 | 265,426 | 623,990 | 626,078 | 2.360 [2.359, 2.362] | 2.359 |
| ref-T / ref | 512 | all-in | 2 | 200,320 | 270,910 | 443,442 | 631,187 | 2.214 [2.208, 2.217] | 2.330 |
| ref-T / ref | 512 | all-in | 4 | 167,124 | 274,498 | 353,133 | 638,578 | 2.113 [2.111, 2.115] | 2.326 |
| ref-T / ref | 512 | all-in | 8 | 152,220 | 286,633 | 310,004 | 658,036 | 2.037 [2.033, 2.039] | 2.296 |
| ref-T / ref | 512 | prepared | 1 | 268,337 | 269,447 | 509,507 | 511,313 | 1.899 [1.897, 1.900] | 1.898 |
| ref-T / ref | 512 | prepared | 2 | 204,173 | 273,842 | 329,139 | 515,565 | 1.612 [1.608, 1.614] | 1.883 |
| ref-T / ref | 512 | prepared | 4 | 170,899 | 275,465 | 238,891 | 521,056 | 1.398 [1.396, 1.399] | 1.892 |
| ref-T / ref | 512 | prepared | 8 | 156,064 | 283,865 | 195,616 | 536,796 | 1.253 [1.251, 1.255] | 1.891 |
| ref-T / ref | 1024 | all-in | 1 | 572,278 | 574,206 | 1,316,054 | 1,319,246 | 2.300 [2.298, 2.302] | 2.298 |
| ref-T / ref | 1024 | all-in | 2 | 441,576 | 580,083 | 950,816 | 1,323,841 | 2.153 [2.153, 2.154] | 2.282 |
| ref-T / ref | 1024 | all-in | 4 | 374,900 | 584,065 | 768,109 | 1,329,690 | 2.049 [2.048, 2.050] | 2.277 |
| ref-T / ref | 1024 | all-in | 8 | 342,994 | 591,621 | 679,529 | 1,344,589 | 1.981 [1.981, 1.982] | 2.273 |
| ref-T / ref | 1024 | prepared | 1 | 550,357 | 552,259 | 1,048,374 | 1,051,119 | 1.905 [1.903, 1.908] | 1.903 |
| ref-T / ref | 1024 | prepared | 2 | 419,491 | 557,918 | 683,459 | 1,055,639 | 1.629 [1.629, 1.630] | 1.892 |
| ref-T / ref | 1024 | prepared | 4 | 352,703 | 561,753 | 500,993 | 1,061,432 | 1.420 [1.420, 1.421] | 1.889 |
| ref-T / ref | 1024 | prepared | 8 | 320,832 | 569,242 | 412,069 | 1,076,098 | 1.284 [1.284, 1.285] | 1.890 |
| hot64 / hot64 | 256 | all-in | 1 | 123,152 | 123,805 | 198,015 | 198,828 | 1.608 [1.606, 1.610] | 1.606 |
| hot64 / hot64 | 256 | all-in | 2 | 94,944 | 128,745 | 162,497 | 203,257 | 1.712 [1.709, 1.714] | 1.579 |
| hot64 / hot64 | 256 | all-in | 4 | 80,995 | 135,144 | 144,927 | 209,926 | 1.789 [1.787, 1.792] | 1.553 |
| hot64 / hot64 | 256 | all-in | 8 | 74,235 | 142,765 | 137,591 | 225,087 | 1.853 [1.852, 1.856] | 1.577 |
| hot64 / hot64 | 256 | prepared | 1 | 126,085 | 126,766 | 143,930 | 144,676 | 1.142 [1.141, 1.143] | 1.141 |
| hot64 / hot64 | 256 | prepared | 2 | 97,834 | 130,683 | 109,065 | 148,445 | 1.115 [1.113, 1.116] | 1.136 |
| hot64 / hot64 | 256 | prepared | 4 | 83,867 | 135,116 | 91,624 | 153,216 | 1.092 [1.091, 1.094] | 1.134 |
| hot64 / hot64 | 256 | prepared | 8 | 77,108 | 138,896 | 84,168 | 164,299 | 1.092 [1.090, 1.093] | 1.183 |
| hot64 / hot64 | 512 | all-in | 1 | 257,174 | 258,223 | 408,500 | 409,975 | 1.588 [1.587, 1.590] | 1.588 |
| hot64 / hot64 | 512 | all-in | 2 | 196,854 | 264,002 | 335,546 | 414,636 | 1.705 [1.703, 1.706] | 1.571 |
| hot64 / hot64 | 512 | all-in | 4 | 165,877 | 269,174 | 298,738 | 419,638 | 1.801 [1.799, 1.803] | 1.559 |
| hot64 / hot64 | 512 | all-in | 8 | 152,323 | 285,953 | 282,089 | 432,936 | 1.852 [1.850, 1.854] | 1.514 |
| hot64 / hot64 | 512 | prepared | 1 | 261,199 | 262,262 | 293,931 | 295,110 | 1.125 [1.125, 1.126] | 1.125 |
| hot64 / hot64 | 512 | prepared | 2 | 200,741 | 266,955 | 221,037 | 298,820 | 1.101 [1.100, 1.102] | 1.119 |
| hot64 / hot64 | 512 | prepared | 4 | 169,652 | 270,127 | 184,401 | 301,983 | 1.087 [1.086, 1.088] | 1.118 |
| hot64 / hot64 | 512 | prepared | 8 | 156,124 | 283,103 | 167,580 | 311,444 | 1.073 [1.072, 1.074] | 1.100 |
| hot64 / hot64 | 1024 | all-in | 1 | 557,753 | 559,592 | 884,218 | 886,659 | 1.585 [1.585, 1.586] | 1.584 |
| hot64 / hot64 | 1024 | all-in | 2 | 434,162 | 565,332 | 734,965 | 891,287 | 1.693 [1.692, 1.693] | 1.577 |
| hot64 / hot64 | 1024 | all-in | 4 | 372,045 | 572,369 | 660,295 | 896,945 | 1.775 [1.774, 1.775] | 1.567 |
| hot64 / hot64 | 1024 | all-in | 8 | 342,142 | 582,888 | 625,108 | 906,788 | 1.827 [1.826, 1.828] | 1.556 |
| hot64 / hot64 | 1024 | prepared | 1 | 535,875 | 537,662 | 616,560 | 618,420 | 1.151 [1.150, 1.151] | 1.150 |
| hot64 / hot64 | 1024 | prepared | 2 | 412,215 | 543,269 | 467,573 | 623,070 | 1.134 [1.134, 1.135] | 1.147 |
| hot64 / hot64 | 1024 | prepared | 4 | 349,925 | 550,093 | 393,269 | 628,726 | 1.124 [1.123, 1.124] | 1.143 |
| hot64 / hot64 | 1024 | prepared | 8 | 320,049 | 560,581 | 357,605 | 638,399 | 1.117 [1.117, 1.118] | 1.139 |
| hot32 / hot32 | 256 | all-in | 1 | 100,792 | 101,384 | 173,350 | 174,136 | 1.720 [1.717, 1.725] | 1.718 |
| hot32 / hot32 | 256 | all-in | 2 | 84,529 | 107,858 | 150,767 | 179,860 | 1.784 [1.781, 1.786] | 1.668 |
| hot32 / hot32 | 256 | all-in | 4 | 75,259 | 112,153 | 139,157 | 186,960 | 1.849 [1.846, 1.852] | 1.667 |
| hot32 / hot32 | 256 | all-in | 8 | 71,475 | 120,738 | 134,693 | 202,460 | 1.884 [1.882, 1.887] | 1.677 |
| hot32 / hot32 | 256 | prepared | 1 | 103,750 | 104,354 | 119,600 | 120,270 | 1.153 [1.151, 1.156] | 1.153 |
| hot32 / hot32 | 256 | prepared | 2 | 87,436 | 109,811 | 97,481 | 125,190 | 1.115 [1.114, 1.116] | 1.140 |
| hot32 / hot32 | 256 | prepared | 4 | 78,140 | 112,184 | 85,940 | 130,410 | 1.100 [1.098, 1.101] | 1.162 |
| hot32 / hot32 | 256 | prepared | 8 | 74,330 | 116,795 | 81,369 | 141,852 | 1.095 [1.093, 1.096] | 1.215 |
| hot32 / hot32 | 512 | all-in | 1 | 212,300 | 213,236 | 359,729 | 361,092 | 1.694 [1.694, 1.696] | 1.693 |
| hot32 / hot32 | 512 | all-in | 2 | 174,030 | 218,178 | 311,423 | 366,192 | 1.789 [1.788, 1.791] | 1.678 |
| hot32 / hot32 | 512 | all-in | 4 | 155,393 | 227,238 | 287,249 | 373,490 | 1.849 [1.846, 1.851] | 1.644 |
| hot32 / hot32 | 512 | all-in | 8 | 146,458 | 239,268 | 276,550 | 387,519 | 1.888 [1.886, 1.891] | 1.620 |
| hot32 / hot32 | 512 | prepared | 1 | 216,333 | 217,297 | 245,160 | 246,190 | 1.133 [1.133, 1.134] | 1.133 |
| hot32 / hot32 | 512 | prepared | 2 | 177,925 | 221,139 | 196,863 | 250,300 | 1.106 [1.105, 1.108] | 1.132 |
| hot32 / hot32 | 512 | prepared | 4 | 159,169 | 228,176 | 172,843 | 255,734 | 1.086 [1.084, 1.088] | 1.121 |
| hot32 / hot32 | 512 | prepared | 8 | 150,268 | 236,420 | 161,749 | 265,880 | 1.076 [1.075, 1.078] | 1.125 |
| hot32 / hot32 | 1024 | all-in | 1 | 468,906 | 470,533 | 786,702 | 789,012 | 1.678 [1.677, 1.678] | 1.677 |
| hot32 / hot32 | 1024 | all-in | 2 | 389,589 | 475,943 | 686,744 | 794,432 | 1.763 [1.761, 1.764] | 1.669 |
| hot32 / hot32 | 1024 | all-in | 4 | 349,280 | 481,315 | 636,409 | 800,717 | 1.822 [1.821, 1.823] | 1.664 |
| hot32 / hot32 | 1024 | all-in | 8 | 331,642 | 499,461 | 613,722 | 814,785 | 1.851 [1.850, 1.852] | 1.631 |
| hot32 / hot32 | 1024 | prepared | 1 | 446,933 | 448,506 | 519,195 | 520,861 | 1.162 [1.161, 1.162] | 1.161 |
| hot32 / hot32 | 1024 | prepared | 2 | 367,633 | 453,929 | 419,409 | 526,230 | 1.141 [1.140, 1.142] | 1.159 |
| hot32 / hot32 | 1024 | prepared | 4 | 327,204 | 459,120 | 369,370 | 532,467 | 1.129 [1.128, 1.130] | 1.160 |
| hot32 / hot32 | 1024 | prepared | 8 | 309,523 | 477,142 | 346,176 | 546,314 | 1.118 [1.118, 1.119] | 1.145 |

## Table 5: Throughput per core: P independent single-thread signers (flags O2, run set headline)

Signatures per second of signing time per process (count / sum of wall_ns), mean over the P processes, which run the same schedule (the same keys, messages and seeds, so the same signatures) at the same time on their own (pinned) CPUs.

| pair | n | acct | P | HAWK sig/s/core | E8 sig/s/core | E8/HAWK [95% CI] |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| ref / ref | 256 | all-in | 8 | 31,613 | 13,867 | 0.439 [0.438, 0.439] |
| ref / ref | 256 | prepared | 8 | 30,411 | 16,932 | 0.557 [0.556, 0.558] |
| ref / ref | 512 | all-in | 8 | 16,668 | 6,847 | 0.411 [0.411, 0.411] |
| ref / ref | 512 | prepared | 8 | 16,363 | 8,383 | 0.512 [0.512, 0.513] |
| ref / ref | 1024 | all-in | 8 | 7,606 | 3,253 | 0.428 [0.427, 0.428] |
| ref / ref | 1024 | prepared | 8 | 7,976 | 4,083 | 0.512 [0.511, 0.512] |
| ref-T / ref | 256 | all-in | 8 | 31,284 | 13,867 | 0.443 [0.443, 0.444] |
| ref-T / ref | 256 | prepared | 8 | 30,243 | 16,932 | 0.560 [0.559, 0.560] |
| ref-T / ref | 512 | all-in | 8 | 16,168 | 6,847 | 0.424 [0.423, 0.424] |
| ref-T / ref | 512 | prepared | 8 | 15,931 | 8,383 | 0.526 [0.526, 0.527] |
| ref-T / ref | 1024 | all-in | 8 | 7,470 | 3,253 | 0.436 [0.435, 0.436] |
| ref-T / ref | 1024 | prepared | 8 | 7,768 | 4,083 | 0.526 [0.525, 0.526] |
| hot64 / hot64 | 256 | all-in | 8 | 31,638 | 21,420 | 0.677 [0.676, 0.679] |
| hot64 / hot64 | 256 | prepared | 8 | 30,576 | 29,713 | 0.972 [0.971, 0.973] |
| hot64 / hot64 | 512 | all-in | 8 | 16,633 | 10,474 | 0.630 [0.629, 0.630] |
| hot64 / hot64 | 512 | prepared | 8 | 16,382 | 14,556 | 0.889 [0.888, 0.889] |
| hot64 / hot64 | 1024 | all-in | 8 | 7,672 | 4,842 | 0.631 [0.631, 0.632] |
| hot64 / hot64 | 1024 | prepared | 8 | 7,987 | 6,943 | 0.869 [0.869, 0.870] |
| hot32 / hot32 | 256 | all-in | 8 | 39,257 | 24,400 | 0.622 [0.620, 0.624] |
| hot32 / hot32 | 256 | prepared | 8 | 37,787 | 35,692 | 0.945 [0.941, 0.947] |
| hot32 / hot32 | 512 | all-in | 8 | 20,135 | 11,882 | 0.590 [0.589, 0.591] |
| hot32 / hot32 | 512 | prepared | 8 | 19,767 | 17,422 | 0.881 [0.878, 0.884] |
| hot32 / hot32 | 1024 | all-in | 8 | 9,125 | 5,438 | 0.596 [0.595, 0.597] |
| hot32 / hot32 | 1024 | prepared | 8 | 9,574 | 8,235 | 0.860 [0.858, 0.861] |

## Table 6: Stage breakdown at the headline variant (ref, one thread, all-in) (flags O2, run set headline)

Profile benches (hawk_sign_profile_bench: upstream HAWK ref; e8_sign_profile_bench: E8 ref): TSC cycles per accepted signature including the per-stage timers, so the totals exceed the timed figures; mean over runs.

| scheme | n | stage | cycles / sign | share | runs |
| --- | ---: | --- | ---: | ---: | ---: |
| HAWK | 256 | key expand | 6,475 | 4.7% | 5 |
| HAWK | 256 | hash msg hpub | 1,370 | 1.0% | 5 |
| HAWK | 256 | salt derivation | 1,818 | 1.3% | 5 |
| HAWK | 256 | challenge hash | 1,778 | 1.3% | 5 |
| HAWK | 256 | target coset | 3,032 | 2.2% | 5 |
| HAWK | 256 | gaussian sampler | 76,654 | 55.4% | 5 |
| HAWK | 256 | norm rejection | 307 | 0.2% | 5 |
| HAWK | 256 | reconstruct s1 | 40,409 | 29.2% | 5 |
| HAWK | 256 | symbreak bounds | 1,936 | 1.4% | 5 |
| HAWK | 256 | encode sig | 3,255 | 2.4% | 5 |
| HAWK | 256 | other | 1,260 | 0.9% | 5 |
| HAWK | 256 | total | 138,294 | 100.0% | 5 |
| HAWK | 512 | key expand | 15,470 | 6.0% | 5 |
| HAWK | 512 | hash msg hpub | 1,381 | 0.5% | 5 |
| HAWK | 512 | salt derivation | 1,598 | 0.6% | 5 |
| HAWK | 512 | challenge hash | 1,667 | 0.6% | 5 |
| HAWK | 512 | target coset | 6,715 | 2.6% | 5 |
| HAWK | 512 | gaussian sampler | 129,119 | 49.9% | 5 |
| HAWK | 512 | norm rejection | 255 | 0.1% | 5 |
| HAWK | 512 | reconstruct s1 | 89,894 | 34.7% | 5 |
| HAWK | 512 | symbreak bounds | 3,548 | 1.4% | 5 |
| HAWK | 512 | encode sig | 8,089 | 3.1% | 5 |
| HAWK | 512 | other | 1,154 | 0.4% | 5 |
| HAWK | 512 | total | 258,890 | 100.0% | 5 |
| HAWK | 1024 | key expand | 59,768 | 10.6% | 5 |
| HAWK | 1024 | hash msg hpub | 1,465 | 0.3% | 5 |
| HAWK | 1024 | salt derivation | 2,774 | 0.5% | 5 |
| HAWK | 1024 | challenge hash | 2,881 | 0.5% | 5 |
| HAWK | 1024 | target coset | 19,209 | 3.4% | 5 |
| HAWK | 1024 | gaussian sampler | 260,689 | 46.0% | 5 |
| HAWK | 1024 | norm rejection | 256 | 0.0% | 5 |
| HAWK | 1024 | reconstruct s1 | 195,255 | 34.5% | 5 |
| HAWK | 1024 | symbreak bounds | 6,747 | 1.2% | 5 |
| HAWK | 1024 | encode sig | 15,923 | 2.8% | 5 |
| HAWK | 1024 | other | 1,172 | 0.2% | 5 |
| HAWK | 1024 | total | 566,140 | 100.0% | 5 |
| E8 | 256 | key prepare | 60,791 | 19.6% | 5 |
| E8 | 256 | salt | 2,693 | 0.9% | 5 |
| E8 | 256 | hash challenge | 3,947 | 1.3% | 5 |
| E8 | 256 | target coset | 6,876 | 2.2% | 5 |
| E8 | 256 | e8 sampler | 188,405 | 60.9% | 5 |
| E8 | 256 | reconstruct s | 43,987 | 14.2% | 5 |
| E8 | 256 | norm rejection | 510 | 0.2% | 5 |
| E8 | 256 | encode sig | 938 | 0.3% | 5 |
| E8 | 256 | other | 1,476 | 0.5% | 5 |
| E8 | 256 | total | 309,622 | 100.0% | 5 |
| E8 | 512 | key prepare | 131,677 | 20.9% | 5 |
| E8 | 512 | salt | 2,755 | 0.4% | 5 |
| E8 | 512 | hash challenge | 4,975 | 0.8% | 5 |
| E8 | 512 | target coset | 18,047 | 2.9% | 5 |
| E8 | 512 | e8 sampler | 374,225 | 59.4% | 5 |
| E8 | 512 | reconstruct s | 94,720 | 15.0% | 5 |
| E8 | 512 | norm rejection | 510 | 0.1% | 5 |
| E8 | 512 | encode sig | 1,426 | 0.2% | 5 |
| E8 | 512 | other | 1,498 | 0.2% | 5 |
| E8 | 512 | total | 629,833 | 100.0% | 5 |
| E8 | 1024 | key prepare | 301,248 | 22.7% | 5 |
| E8 | 1024 | salt | 3,888 | 0.3% | 5 |
| E8 | 1024 | hash challenge | 8,145 | 0.6% | 5 |
| E8 | 1024 | target coset | 58,559 | 4.4% | 5 |
| E8 | 1024 | e8 sampler | 748,620 | 56.4% | 5 |
| E8 | 1024 | reconstruct s | 201,628 | 15.2% | 5 |
| E8 | 1024 | norm rejection | 511 | 0.0% | 5 |
| E8 | 1024 | encode sig | 2,511 | 0.2% | 5 |
| E8 | 1024 | other | 1,549 | 0.1% | 5 |
| E8 | 1024 | total | 1,326,659 | 100.0% | 5 |

## Table 7: Context: HAWK AVX2 (upstream Optimized_Implementation; not a fair pair) (flags O2, run set headline)

Implementation context only: E8 has no AVX2 path, so no claim about the lattice rests on this table. Primary cycles, one thread, same harness and flag set.

| n | acct | HAWK ref | HAWK ref-avx2 | AVX2 / ref | E8 ref | E8 ref / HAWK AVX2 [95% CI] |
| ---: | --- | ---: | ---: | ---: | ---: | --- |
| 256 | all-in | 122,975 | 38,114 | 0.310 | 307,484 | 8.068 [8.052, 8.082] |
| 256 | prepared | 126,837 | 45,962 | 0.362 | 253,226 | 5.510 [5.503, 5.516] |
| 512 | all-in | 257,459 | 68,333 | 0.265 | 626,078 | 9.162 [9.154, 9.175] |
| 512 | prepared | 262,320 | 84,092 | 0.321 | 511,313 | 6.080 [6.076, 6.086] |
| 1024 | all-in | 565,264 | 144,202 | 0.255 | 1,319,246 | 9.149 [9.138, 9.160] |
| 1024 | prepared | 539,112 | 161,178 | 0.299 | 1,051,119 | 6.521 [6.514, 6.529] |

## Table 8: Key generation: cycles per key (flags O2, run set headline)

Each scheme's public keygen call (RNG to encoded private and public key, retries included), one thread, pinned, interleaved per key index in rotated order after warm-up. HAWK, E8 and the core (Hawk_keygen() alone: HAWK's NTRU core, which both call) draw from the same paired seed per key index. mean: mean of the per-run means over key indices (key generation restarts, so its expected cost is the mean); median: mean of the per-run medians. E8/HAWK: ratio of means, 95% CI from a paired bootstrap (runs, then key indices). tail: the scheme's mean minus the core's on the same key indices (HAWK: public-key encoding; E8: its public form, key generation limits and invertibility check over 3 primes; HAWK's own form and range check run inside the core for both). diverged: share of key indices where HAWK and E8 made different numbers of RNG calls (one restarted after the core). draws: (f, g) draws (RNG calls) per key. Key bytes from sizes.csv. HAWK AVX2 rows (ref-avx2): context, not a fair pair (E8 / HAWK AVX2 in its ratio column).

| n | scheme | variant | mean | median | E8/HAWK [95% CI] | tail | diverged | draws | pub B | priv B | runs x keys |
| ---: | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | HAWK | ref | 2,593,618 | 2,405,756 |  | 24,744 |  | 73.1 | 450 | 96 | 5 x 1000 |
| 256 | E8 | ref | 2,670,953 | 2,495,785 | 1.030 [1.027, 1.032] | 102,078 | 0.5% | 72.6 | 2,048 | 544 | 5 x 1000 |
| 256 | core | ref | 2,568,874 | 2,394,070 |  |  |  | 72.6 |  |  | 5 x 1000 |
| 256 | HAWK | ref-avx2 | 1,278,598 | 1,190,932 | 2.089 [2.083, 2.095] | 17,485 |  | 73.1 | 450 | 96 | 5 x 1000 |
| 256 | core | ref-avx2 | 1,261,113 | 1,181,211 |  |  |  | 72.6 |  |  | 5 x 1000 |
| 512 | HAWK | ref | 9,341,407 | 7,876,356 |  | 791,973 |  | 18.7 | 1,024 | 184 | 5 x 1000 |
| 512 | E8 | ref | 8,766,702 | 8,039,082 | 0.938 [0.931, 0.946] | 217,268 | 8.5% | 17.3 | 4,096 | 1,080 | 5 x 1000 |
| 512 | core | ref | 8,549,434 | 7,821,119 |  |  |  | 17.3 |  |  | 5 x 1000 |
| 512 | HAWK | ref-avx2 | 5,469,503 | 4,423,050 | 1.603 [1.589, 1.616] | 467,778 |  | 18.7 | 1,024 | 184 | 5 x 1000 |
| 512 | core | ref-avx2 | 5,001,725 | 4,388,933 |  |  |  | 17.3 |  |  | 5 x 1000 |
| 1024 | HAWK | ref | 49,465,070 | 46,300,605 |  | 853,575 |  | 10.6 | 2,440 | 360 | 5 x 1000 |
| 1024 | E8 | ref | 49,073,483 | 40,771,066 | 0.992 [0.988, 0.996] | 461,988 | 1.6% | 10.4 | 8,192 | 2,152 | 5 x 1000 |
| 1024 | core | ref | 48,611,495 | 40,317,318 |  |  |  | 10.4 |  |  | 5 x 1000 |
| 1024 | HAWK | ref-avx2 | 34,191,304 | 32,472,538 | 1.435 [1.427, 1.443] | 606,720 |  | 10.6 | 2,440 | 360 | 5 x 1000 |
| 1024 | core | ref-avx2 | 33,584,584 | 26,175,592 |  |  |  | 10.4 |  |  | 5 x 1000 |

## Table 9: Verification: cycles per signature (flags O2, run set headline)

One thread, pinned, interleaved per signature in rotated order after warm-up; inputs: the paired-stream keys, each scheme's ref signer, all-in, T = 1 (verification cost does not depend on the sampler). all-in: encoded signature and public key (decoding and hashing inside the timed call; HAWK with HAWK_TMPSIZE_VERIFY_FAST, E8 with E8_TMPSIZE_VERIFY); prepared: the public key decoded once per key outside the timed call (decode only, for both; timed as pub dec), the signature still encoded. Estimate: median per run, mean over runs; E8/HAWK with its bootstrap 95% CI. primes: NTT primes of the norm check, each scheme the minimum its own HAWK-style range argument proves sound: HAWK 2, E8 3, because the E8-CM public form (paper Sec. 4.5) has coefficients 2^3 to 2^7 times larger than HAWK's (e8/docs/verifier_range.md); a cost of the lattice. E8 fast has HAWK's verifier techniques (precomputed twiddles, constant NTT of Delta, batch inversion, integer bound, early reject). E8 ref (4 primes, twiddles and Delta rebuilt per call, one Fermat inversion per coefficient, double bound) is the verifier the submission measured, as context. HAWK reconstructs s0 from s1 (its compressed signature carries s1 only); E8's uncompressed signature carries s0 (about 4 times larger), so E8 does not. ref-avx2: HAWK AVX2 context (E8 fast / HAWK AVX2 in its ratio column).

| n | acct | scheme | variant | verifier | primes | cycles | TSC | E8/HAWK [95% CI] | pub dec | sig B | pub B |
| ---: | --- | --- | --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| 256 | all-in | HAWK | ref | upstream | 2 | 115,740 | 115,094 |  | 11,792 | 249 | 450 |
| 256 | all-in | E8 | ref | fast | 3 | 158,249 | 157,466 | 1.367 [1.366, 1.368] | 24,095 | 1,038 | 2,048 |
| 256 | all-in | E8 | ref | ref | 4 | 771,079 | 768,462 | 6.662 [6.655, 6.667] | 24,095 | 1,038 | 2,048 |
| 256 | all-in | HAWK | ref-avx2 | upstream | 2 | 40,245 | 39,809 | 3.932 [3.931, 3.933] | 7,674 | 249 | 450 |
| 256 | prepared | HAWK | ref | upstream | 2 | 105,888 | 105,255 |  | 11,792 | 249 | 450 |
| 256 | prepared | E8 | ref | fast | 3 | 135,439 | 134,736 | 1.279 [1.278, 1.280] | 24,095 | 1,038 | 2,048 |
| 256 | prepared | E8 | ref | ref | 4 | 741,952 | 739,505 | 7.007 [7.001, 7.011] | 24,095 | 1,038 | 2,048 |
| 256 | prepared | HAWK | ref-avx2 | upstream | 2 | 33,468 | 33,067 | 4.047 [4.045, 4.048] | 7,674 | 249 | 450 |
| 512 | all-in | HAWK | ref | upstream | 2 | 247,376 | 246,364 |  | 24,050 | 555 | 1,024 |
| 512 | all-in | E8 | ref | fast | 3 | 331,723 | 330,446 | 1.341 [1.341, 1.341] | 45,504 | 2,072 | 4,096 |
| 512 | all-in | E8 | ref | ref | 4 | 1,560,822 | 1,556,157 | 6.310 [6.308, 6.311] | 45,504 | 2,072 | 4,096 |
| 512 | all-in | HAWK | ref-avx2 | upstream | 2 | 78,553 | 78,019 | 4.223 [4.221, 4.224] | 14,715 | 555 | 1,024 |
| 512 | prepared | HAWK | ref | upstream | 2 | 226,151 | 225,191 |  | 24,050 | 555 | 1,024 |
| 512 | prepared | E8 | ref | fast | 3 | 287,497 | 286,354 | 1.271 [1.271, 1.272] | 45,504 | 2,072 | 4,096 |
| 512 | prepared | E8 | ref | ref | 4 | 1,519,913 | 1,515,393 | 6.721 [6.718, 6.723] | 45,504 | 2,072 | 4,096 |
| 512 | prepared | HAWK | ref-avx2 | upstream | 2 | 64,627 | 64,139 | 4.449 [4.448, 4.449] | 14,715 | 555 | 1,024 |
| 1024 | all-in | HAWK | ref | upstream | 2 | 530,750 | 529,038 |  | 53,383 | 1,221 | 2,440 |
| 1024 | all-in | E8 | ref | fast | 3 | 697,794 | 695,568 | 1.315 [1.314, 1.315] | 87,366 | 4,136 | 8,192 |
| 1024 | all-in | E8 | ref | ref | 4 | 3,201,297 | 3,197,377 | 6.032 [6.029, 6.034] | 87,366 | 4,136 | 8,192 |
| 1024 | all-in | HAWK | ref-avx2 | upstream | 2 | 161,625 | 160,794 | 4.317 [4.316, 4.318] | 31,617 | 1,221 | 2,440 |
| 1024 | prepared | HAWK | ref | upstream | 2 | 484,958 | 483,320 |  | 53,383 | 1,221 | 2,440 |
| 1024 | prepared | E8 | ref | fast | 3 | 610,823 | 608,837 | 1.260 [1.259, 1.260] | 87,366 | 4,136 | 8,192 |
| 1024 | prepared | E8 | ref | ref | 4 | 3,120,796 | 3,116,984 | 6.435 [6.433, 6.437] | 87,366 | 4,136 | 8,192 |
| 1024 | prepared | HAWK | ref-avx2 | upstream | 2 | 131,056 | 130,342 | 4.661 [4.660, 4.662] | 31,617 | 1,221 | 2,440 |

## Table 1: Headline, one core, as-specified precision: HAWK ref vs E8 ref (flags O3native, run set headline)

cycles: core cycles (primary); TSC and us secondary. Estimates: median per run, mean over runs. all-in: from the encoded private key; prepared: from a prepared key (HAWK decoded, E8 prepared basis; preparation timed separately). attempts, sig B (encoded signature bytes: HAWK compressed, E8 uncompressed) and rng B (random bytes) per signature. Precision: threshold bits, per-signature SD bound (log2), RD loss factor at the optimal order and eq. (6) of the HAWK spec (e8/docs/precision_summary.csv).

| n | acct | scheme | variant | cycles | TSC | us | attempts | sig B | rng B | bits | SD log2 | loss | eq6 | E8/HAWK [95% CI] |
| ---: | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 256 | all-in | HAWK | ref | 109,956 | 109,340 | 25.5 | 1.1853 | 249 | 6,133 | 78 | -67.594 | 1.0001 | yes |  |
| 256 | all-in | E8 | ref | 200,999 | 200,105 | 46.7 | 1.0000 | 1,038 | 5,814 | 78 | -67.057 | 1.0000 | yes | 1.828 [1.824, 1.831] |
| 256 | prepared | HAWK | ref | 112,839 | 112,213 | 26.2 | 1.1822 | 249 | 6,117 | 78 | -67.594 | 1.0001 | yes |  |
| 256 | prepared | E8 | ref | 159,540 | 158,773 | 37.0 | 1.0000 | 1,038 | 5,814 | 78 | -67.057 | 1.0000 | yes | 1.414 [1.410, 1.417] |
| 512 | all-in | HAWK | ref | 219,704 | 218,767 | 51.0 | 1.0000 | 555 | 10,304 | 78 | -66.836 | 1.0602 | yes |  |
| 512 | all-in | E8 | ref | 401,158 | 399,681 | 93.2 | 1.0000 | 2,072 | 11,584 | 78 | -66.157 | 1.0290 | yes | 1.826 [1.826, 1.826] |
| 512 | prepared | HAWK | ref | 223,336 | 222,396 | 51.9 | 1.0000 | 555 | 10,304 | 78 | -66.836 | 1.0602 | yes |  |
| 512 | prepared | E8 | ref | 316,548 | 315,336 | 73.5 | 1.0000 | 2,072 | 11,584 | 78 | -66.157 | 1.0290 | yes | 1.417 [1.417, 1.418] |
| 1024 | all-in | HAWK | ref | 468,794 | 467,169 | 108.9 | 1.0000 | 1,221 | 20,560 | 78 | -65.229 | 1.1674 | yes |  |
| 1024 | all-in | E8 | ref | 836,791 | 834,260 | 194.4 | 1.0000 | 4,136 | 23,120 | 78 | -65.256 | 1.1251 | yes | 1.785 [1.783, 1.787] |
| 1024 | prepared | HAWK | ref | 445,186 | 443,631 | 103.4 | 1.0000 | 1,221 | 20,560 | 78 | -65.229 | 1.1674 | yes |  |
| 1024 | prepared | E8 | ref | 644,580 | 642,497 | 149.7 | 1.0000 | 4,136 | 23,120 | 78 | -65.256 | 1.1251 | yes | 1.448 [1.446, 1.450] |

## Table 2: Same technique, one core: hot64 and hot32 (flags O3native, run set headline)

cycles: core cycles (primary); TSC and us secondary. Estimates: median per run, mean over runs. all-in: from the encoded private key; prepared: from a prepared key (HAWK decoded, E8 prepared basis; preparation timed separately). attempts, sig B (encoded signature bytes: HAWK compressed, E8 uncompressed) and rng B (random bytes) per signature. Precision: threshold bits, per-signature SD bound (log2), RD loss factor at the optimal order and eq. (6) of the HAWK spec (e8/docs/precision_summary.csv).

| n | acct | scheme | variant | cycles | TSC | us | attempts | sig B | rng B | bits | SD log2 | loss | eq6 | E8/HAWK [95% CI] |
| ---: | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 256 | all-in | HAWK | hot64 | 111,202 | 110,579 | 25.8 | 1.1762 | 249 | 4,898 | 64 | -54.585 | 1.0002 | yes |  |
| 256 | all-in | E8 | hot64 | 167,256 | 166,444 | 38.8 | 1.0000 | 1,038 | 4,662 | 64 | -54.130 | 1.0000 | yes | 1.504 [1.503, 1.505] |
| 256 | prepared | HAWK | hot64 | 113,123 | 112,497 | 26.3 | 1.1785 | 249 | 4,908 | 64 | -54.585 | 1.0002 | yes |  |
| 256 | prepared | E8 | hot64 | 125,779 | 125,096 | 29.2 | 1.0000 | 1,038 | 4,662 | 64 | -54.130 | 1.0000 | yes | 1.112 [1.111, 1.113] |
| 512 | all-in | HAWK | hot64 | 223,101 | 222,181 | 51.8 | 1.0000 | 555 | 8,423 | 64 | -53.386 | 1.8e+09 | no |  |
| 512 | all-in | E8 | hot64 | 334,568 | 333,293 | 77.7 | 1.0000 | 2,072 | 9,280 | 64 | -53.040 | 1.0036 | yes | 1.500 [1.498, 1.501] |
| 512 | prepared | HAWK | hot64 | 225,792 | 224,856 | 52.4 | 1.0000 | 555 | 8,423 | 64 | -53.386 | 1.8e+09 | no |  |
| 512 | prepared | E8 | hot64 | 250,038 | 248,987 | 58.1 | 1.0000 | 2,072 | 9,280 | 64 | -53.040 | 1.0036 | yes | 1.107 [1.106, 1.108] |
| 1024 | all-in | HAWK | hot64 | 468,222 | 466,645 | 108.8 | 1.0000 | 1,221 | 16,832 | 64 | -52.858 | 1.9e+15 | no |  |
| 1024 | all-in | E8 | hot64 | 701,000 | 698,845 | 162.9 | 1.0000 | 4,136 | 18,512 | 64 | -51.732 | 1.0061 | yes | 1.497 [1.497, 1.498] |
| 1024 | prepared | HAWK | hot64 | 448,663 | 447,166 | 104.2 | 1.0000 | 1,221 | 16,832 | 64 | -52.858 | 1.9e+15 | no |  |
| 1024 | prepared | E8 | hot64 | 509,225 | 507,495 | 118.3 | 1.0000 | 4,136 | 18,512 | 64 | -51.732 | 1.0061 | yes | 1.135 [1.134, 1.136] |
| 256 | all-in | HAWK | hot32 | 89,342 | 88,786 | 20.7 | 1.1753 | 249 | 2,479 | 32 | -22.841 | 7.8980 | no |  |
| 256 | all-in | E8 | hot32 | 143,918 | 143,173 | 33.4 | 1.0000 | 1,038 | 2,358 | 32 | -21.886 | 16.8477 | no | 1.611 [1.609, 1.612] |
| 256 | prepared | HAWK | hot32 | 91,251 | 90,696 | 21.2 | 1.1702 | 249 | 2,468 | 32 | -22.841 | 7.8980 | no |  |
| 256 | prepared | E8 | hot32 | 102,455 | 101,833 | 23.8 | 1.0000 | 1,038 | 2,358 | 32 | -21.886 | 16.8477 | no | 1.123 [1.122, 1.123] |
| 512 | all-in | HAWK | hot32 | 179,889 | 179,078 | 41.8 | 1.0000 | 555 | 4,244 | 32 | -21.402 | inf | no |  |
| 512 | all-in | E8 | hot32 | 287,887 | 286,767 | 66.9 | 1.0000 | 2,072 | 4,672 | 32 | -20.726 | inf | no | 1.600 [1.600, 1.601] |
| 512 | prepared | HAWK | hot32 | 182,570 | 181,752 | 42.4 | 1.0000 | 555 | 4,244 | 32 | -21.402 | inf | no |  |
| 512 | prepared | E8 | hot32 | 203,381 | 202,470 | 47.2 | 1.0000 | 2,072 | 4,672 | 32 | -20.726 | inf | no | 1.114 [1.114, 1.114] |
| 1024 | all-in | HAWK | hot32 | 383,012 | 381,616 | 89.0 | 1.0000 | 1,221 | 8,456 | 32 | -20.221 | inf | no |  |
| 1024 | all-in | E8 | hot32 | 607,401 | 605,483 | 141.1 | 1.0000 | 4,136 | 9,296 | 32 | -19.959 | inf | no | 1.586 [1.585, 1.587] |
| 1024 | prepared | HAWK | hot32 | 363,455 | 362,120 | 84.4 | 1.0000 | 1,221 | 8,456 | 32 | -20.221 | inf | no |  |
| 1024 | prepared | E8 | hot32 | 415,648 | 414,185 | 96.5 | 1.0000 | 4,136 | 9,296 | 32 | -19.959 | inf | no | 1.144 [1.143, 1.144] |

## Table 3: Sampler only, one thread: cycles and random bytes per 8 coordinates (flags O3native, run set headline)

e8_sampler_bench --variant-matrix: median per call over its trials, divided by n/4 (2n coordinates per call); bytes include the 40-byte seed; mean over runs.

| scheme | variant | bits | technique | n | cycles / 8 | TSC / 8 | bytes / 8 | runs |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| HAWK | ref | 78 | ct full scan | 256 | 981.9 | 973.2 | 80.6 | 5 |
| HAWK | ref-T | 78 | ct full scan | 256 | 1153.4 | 1144.1 | 80.6 | 5 |
| HAWK | hot64 | 64 | vt hot table | 256 | 1008.4 | 999.8 | 64.8 | 5 |
| HAWK | hot32 | 32 | vt hot table | 256 | 662.7 | 655.3 | 32.7 | 5 |
| E8 | ref | 78 | ct full scan | 256 | 1650.3 | 1639.1 | 90.6 | 5 |
| E8 | hot64 | 64 | vt hot table | 256 | 1096.6 | 1087.4 | 72.6 | 5 |
| E8 | hot32 | 32 | vt hot table | 256 | 715.0 | 707.5 | 36.6 | 5 |
| HAWK | ref | 78 | ct full scan | 512 | 983.7 | 977.4 | 80.3 | 5 |
| HAWK | ref-T | 78 | ct full scan | 512 | 1143.0 | 1136.0 | 80.3 | 5 |
| HAWK | hot64 | 64 | vt hot table | 512 | 1007.3 | 1000.9 | 65.6 | 5 |
| HAWK | hot32 | 32 | vt hot table | 512 | 671.2 | 666.2 | 33.0 | 5 |
| E8 | ref | 78 | ct full scan | 512 | 1620.9 | 1612.0 | 90.3 | 5 |
| E8 | hot64 | 64 | vt hot table | 512 | 1068.3 | 1061.4 | 72.3 | 5 |
| E8 | hot32 | 32 | vt hot table | 512 | 696.9 | 691.6 | 36.3 | 5 |
| HAWK | ref | 78 | ct full scan | 1024 | 995.0 | 989.7 | 80.2 | 5 |
| HAWK | ref-T | 78 | ct full scan | 1024 | 1127.0 | 1121.6 | 80.2 | 5 |
| HAWK | hot64 | 64 | vt hot table | 1024 | 988.5 | 983.7 | 65.6 | 5 |
| HAWK | hot32 | 32 | vt hot table | 1024 | 656.1 | 652.4 | 32.9 | 5 |
| E8 | ref | 78 | ct full scan | 1024 | 1612.2 | 1605.0 | 90.2 | 5 |
| E8 | hot64 | 64 | vt hot table | 1024 | 1067.2 | 1062.0 | 72.2 | 5 |
| E8 | hot32 | 32 | vt hot table | 1024 | 690.7 | 687.0 | 36.2 | 5 |

## Table 4: Parallel latency: wall-clock and total CPU cycles per signature (flags O3native, run set headline)

TSC: wall-clock cycles of the signing call; core: core cycles summed over the caller and the pool workers' jobs (total CPU cycles; empty if the counter is unavailable). HAWK ref-T is HAWK's split ref on the same bound worker streams as E8.

| pair | n | acct | T | HAWK TSC | HAWK core | E8 TSC | E8 core | E8/HAWK TSC [95% CI] | E8/HAWK core |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| ref-T / ref | 256 | all-in | 1 | 120,047 | 120,688 | 200,105 | 200,999 | 1.667 [1.666, 1.668] | 1.665 |
| ref-T / ref | 256 | all-in | 2 | 87,109 | 124,897 | 153,020 | 207,555 | 1.757 [1.755, 1.758] | 1.662 |
| ref-T / ref | 256 | all-in | 4 | 70,709 | 130,948 | 129,370 | 217,493 | 1.830 [1.828, 1.831] | 1.661 |
| ref-T / ref | 256 | all-in | 8 | 62,694 | 137,863 | 117,493 | 227,184 | 1.874 [1.873, 1.876] | 1.648 |
| ref-T / ref | 256 | prepared | 1 | 121,982 | 122,617 | 158,773 | 159,540 | 1.302 [1.300, 1.304] | 1.301 |
| ref-T / ref | 256 | prepared | 2 | 89,027 | 125,859 | 111,826 | 165,252 | 1.256 [1.255, 1.257] | 1.313 |
| ref-T / ref | 256 | prepared | 4 | 72,567 | 129,893 | 88,150 | 173,212 | 1.215 [1.214, 1.216] | 1.333 |
| ref-T / ref | 256 | prepared | 8 | 64,552 | 132,914 | 76,153 | 178,888 | 1.180 [1.179, 1.180] | 1.346 |
| ref-T / ref | 512 | all-in | 1 | 238,813 | 239,803 | 399,681 | 401,158 | 1.674 [1.673, 1.674] | 1.673 |
| ref-T / ref | 512 | all-in | 2 | 170,246 | 244,687 | 301,912 | 407,439 | 1.773 [1.772, 1.774] | 1.665 |
| ref-T / ref | 512 | all-in | 4 | 135,097 | 248,000 | 252,410 | 414,966 | 1.868 [1.867, 1.869] | 1.673 |
| ref-T / ref | 512 | all-in | 8 | 119,076 | 259,651 | 229,586 | 434,032 | 1.928 [1.927, 1.929] | 1.672 |
| ref-T / ref | 512 | prepared | 1 | 241,505 | 242,499 | 315,336 | 316,548 | 1.306 [1.305, 1.307] | 1.305 |
| ref-T / ref | 512 | prepared | 2 | 172,912 | 246,407 | 217,812 | 322,140 | 1.260 [1.259, 1.260] | 1.307 |
| ref-T / ref | 512 | prepared | 4 | 137,643 | 247,639 | 168,371 | 327,892 | 1.223 [1.222, 1.224] | 1.324 |
| ref-T / ref | 512 | prepared | 8 | 121,699 | 255,394 | 145,177 | 343,016 | 1.193 [1.192, 1.194] | 1.343 |
| ref-T / ref | 1024 | all-in | 1 | 500,520 | 502,232 | 834,260 | 836,791 | 1.667 [1.665, 1.668] | 1.666 |
| ref-T / ref | 1024 | all-in | 2 | 362,447 | 510,272 | 632,917 | 840,565 | 1.746 [1.745, 1.747] | 1.647 |
| ref-T / ref | 1024 | all-in | 4 | 291,084 | 514,247 | 532,581 | 847,389 | 1.830 [1.829, 1.830] | 1.648 |
| ref-T / ref | 1024 | all-in | 8 | 256,934 | 521,494 | 484,060 | 861,661 | 1.884 [1.880, 1.888] | 1.652 |
| ref-T / ref | 1024 | prepared | 1 | 480,972 | 482,640 | 642,497 | 644,580 | 1.336 [1.335, 1.337] | 1.336 |
| ref-T / ref | 1024 | prepared | 2 | 342,959 | 490,695 | 440,526 | 647,690 | 1.284 [1.284, 1.285] | 1.320 |
| ref-T / ref | 1024 | prepared | 4 | 271,433 | 494,441 | 340,302 | 654,375 | 1.254 [1.253, 1.254] | 1.323 |
| ref-T / ref | 1024 | prepared | 8 | 237,317 | 501,627 | 291,738 | 669,051 | 1.229 [1.226, 1.232] | 1.334 |
| hot64 / hot64 | 256 | all-in | 1 | 110,579 | 111,202 | 166,444 | 167,256 | 1.505 [1.504, 1.506] | 1.504 |
| hot64 / hot64 | 256 | all-in | 2 | 82,612 | 115,918 | 135,149 | 171,626 | 1.636 [1.635, 1.637] | 1.481 |
| hot64 / hot64 | 256 | all-in | 4 | 69,024 | 122,520 | 119,832 | 178,856 | 1.736 [1.734, 1.738] | 1.460 |
| hot64 / hot64 | 256 | all-in | 8 | 62,247 | 129,576 | 113,262 | 192,628 | 1.820 [1.818, 1.821] | 1.487 |
| hot64 / hot64 | 256 | prepared | 1 | 112,497 | 113,123 | 125,096 | 125,779 | 1.112 [1.111, 1.113] | 1.112 |
| hot64 / hot64 | 256 | prepared | 2 | 84,504 | 116,822 | 93,938 | 129,297 | 1.112 [1.111, 1.112] | 1.107 |
| hot64 / hot64 | 256 | prepared | 4 | 70,855 | 121,354 | 78,621 | 134,573 | 1.110 [1.109, 1.110] | 1.109 |
| hot64 / hot64 | 256 | prepared | 8 | 64,096 | 124,550 | 71,879 | 144,296 | 1.121 [1.121, 1.122] | 1.159 |
| hot64 / hot64 | 512 | all-in | 1 | 222,181 | 223,101 | 333,293 | 334,568 | 1.500 [1.499, 1.501] | 1.500 |
| hot64 / hot64 | 512 | all-in | 2 | 162,127 | 228,420 | 267,658 | 338,654 | 1.651 [1.650, 1.652] | 1.483 |
| hot64 / hot64 | 512 | all-in | 4 | 131,528 | 233,526 | 234,823 | 343,551 | 1.785 [1.784, 1.786] | 1.471 |
| hot64 / hot64 | 512 | all-in | 8 | 118,147 | 250,643 | 220,169 | 357,051 | 1.864 [1.863, 1.864] | 1.425 |
| hot64 / hot64 | 512 | prepared | 1 | 224,856 | 225,792 | 248,987 | 250,038 | 1.107 [1.106, 1.108] | 1.107 |
| hot64 / hot64 | 512 | prepared | 2 | 164,776 | 230,077 | 183,610 | 253,423 | 1.114 [1.114, 1.115] | 1.101 |
| hot64 / hot64 | 512 | prepared | 4 | 134,065 | 233,142 | 150,801 | 256,494 | 1.125 [1.124, 1.126] | 1.100 |
| hot64 / hot64 | 512 | prepared | 8 | 120,753 | 246,317 | 135,768 | 266,056 | 1.124 [1.124, 1.125] | 1.080 |
| hot64 / hot64 | 1024 | all-in | 1 | 466,645 | 468,222 | 698,845 | 701,000 | 1.498 [1.497, 1.498] | 1.497 |
| hot64 / hot64 | 1024 | all-in | 2 | 344,499 | 474,295 | 565,493 | 705,816 | 1.641 [1.641, 1.642] | 1.488 |
| hot64 / hot64 | 1024 | all-in | 4 | 282,794 | 481,043 | 498,516 | 711,092 | 1.763 [1.762, 1.764] | 1.478 |
| hot64 / hot64 | 1024 | all-in | 8 | 253,021 | 490,944 | 466,567 | 720,316 | 1.844 [1.842, 1.846] | 1.467 |
| hot64 / hot64 | 1024 | prepared | 1 | 447,166 | 448,663 | 507,495 | 509,225 | 1.135 [1.134, 1.136] | 1.135 |
| hot64 / hot64 | 1024 | prepared | 2 | 324,977 | 454,684 | 374,113 | 513,939 | 1.151 [1.150, 1.152] | 1.130 |
| hot64 / hot64 | 1024 | prepared | 4 | 263,186 | 461,291 | 307,080 | 519,183 | 1.167 [1.166, 1.168] | 1.126 |
| hot64 / hot64 | 1024 | prepared | 8 | 233,395 | 471,156 | 274,598 | 528,283 | 1.177 [1.176, 1.177] | 1.121 |
| hot32 / hot32 | 256 | all-in | 1 | 88,786 | 89,342 | 143,173 | 143,918 | 1.613 [1.611, 1.614] | 1.611 |
| hot32 / hot32 | 256 | all-in | 2 | 72,584 | 95,802 | 124,034 | 149,334 | 1.709 [1.707, 1.711] | 1.559 |
| hot32 / hot32 | 256 | all-in | 4 | 63,365 | 99,977 | 114,234 | 156,473 | 1.803 [1.801, 1.805] | 1.565 |
| hot32 / hot32 | 256 | all-in | 8 | 59,503 | 108,282 | 110,553 | 171,286 | 1.858 [1.856, 1.860] | 1.582 |
| hot32 / hot32 | 256 | prepared | 1 | 90,696 | 91,251 | 101,833 | 102,455 | 1.123 [1.122, 1.124] | 1.123 |
| hot32 / hot32 | 256 | prepared | 2 | 74,485 | 96,729 | 82,818 | 106,999 | 1.112 [1.111, 1.113] | 1.106 |
| hot32 / hot32 | 256 | prepared | 4 | 65,231 | 98,896 | 72,997 | 112,161 | 1.119 [1.118, 1.120] | 1.134 |
| hot32 / hot32 | 256 | prepared | 8 | 61,344 | 103,216 | 69,204 | 122,998 | 1.128 [1.127, 1.129] | 1.192 |
| hot32 / hot32 | 512 | all-in | 1 | 179,078 | 179,889 | 286,767 | 287,887 | 1.601 [1.601, 1.602] | 1.600 |
| hot32 / hot32 | 512 | all-in | 2 | 140,300 | 184,596 | 244,309 | 291,803 | 1.741 [1.740, 1.742] | 1.581 |
| hot32 / hot32 | 512 | all-in | 4 | 121,630 | 193,746 | 223,806 | 299,323 | 1.840 [1.839, 1.841] | 1.545 |
| hot32 / hot32 | 512 | all-in | 8 | 112,548 | 205,582 | 214,587 | 312,460 | 1.907 [1.906, 1.908] | 1.520 |
| hot32 / hot32 | 512 | prepared | 1 | 181,752 | 182,570 | 202,470 | 203,381 | 1.114 [1.114, 1.114] | 1.114 |
| hot32 / hot32 | 512 | prepared | 2 | 142,958 | 186,282 | 160,270 | 206,581 | 1.121 [1.120, 1.122] | 1.109 |
| hot32 / hot32 | 512 | prepared | 4 | 124,150 | 193,345 | 139,793 | 212,272 | 1.126 [1.125, 1.127] | 1.098 |
| hot32 / hot32 | 512 | prepared | 8 | 115,111 | 201,283 | 130,204 | 221,476 | 1.131 [1.130, 1.132] | 1.100 |
| hot32 / hot32 | 1024 | all-in | 1 | 381,616 | 383,012 | 605,483 | 607,401 | 1.587 [1.586, 1.587] | 1.586 |
| hot32 / hot32 | 1024 | all-in | 2 | 301,525 | 388,097 | 518,778 | 612,032 | 1.721 [1.720, 1.721] | 1.577 |
| hot32 / hot32 | 1024 | all-in | 4 | 260,993 | 393,561 | 475,313 | 617,630 | 1.821 [1.820, 1.822] | 1.569 |
| hot32 / hot32 | 1024 | all-in | 8 | 243,070 | 411,640 | 455,671 | 631,916 | 1.875 [1.873, 1.876] | 1.535 |
| hot32 / hot32 | 1024 | prepared | 1 | 362,120 | 363,455 | 414,185 | 415,648 | 1.144 [1.143, 1.144] | 1.144 |
| hot32 / hot32 | 1024 | prepared | 2 | 282,011 | 368,491 | 327,333 | 420,139 | 1.161 [1.160, 1.161] | 1.140 |
| hot32 / hot32 | 1024 | prepared | 4 | 241,462 | 373,907 | 283,787 | 425,676 | 1.175 [1.175, 1.176] | 1.138 |
| hot32 / hot32 | 1024 | prepared | 8 | 223,445 | 391,814 | 263,728 | 439,910 | 1.180 [1.179, 1.181] | 1.123 |

## Table 5: Throughput per core: P independent single-thread signers (flags O3native, run set headline)

Signatures per second of signing time per process (count / sum of wall_ns), mean over the P processes, which run the same schedule (the same keys, messages and seeds, so the same signatures) at the same time on their own (pinned) CPUs.

| pair | n | acct | P | HAWK sig/s/core | E8 sig/s/core | E8/HAWK [95% CI] |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| ref / ref | 256 | all-in | 8 | 35,104 | 21,336 | 0.608 [0.607, 0.609] |
| ref / ref | 256 | prepared | 8 | 33,859 | 26,879 | 0.794 [0.793, 0.795] |
| ref / ref | 512 | all-in | 8 | 19,536 | 10,696 | 0.548 [0.547, 0.548] |
| ref / ref | 512 | prepared | 8 | 19,217 | 13,547 | 0.705 [0.705, 0.705] |
| ref / ref | 1024 | all-in | 8 | 9,160 | 5,127 | 0.560 [0.560, 0.560] |
| ref / ref | 1024 | prepared | 8 | 9,645 | 6,653 | 0.690 [0.690, 0.690] |
| ref-T / ref | 256 | all-in | 8 | 31,990 | 21,336 | 0.667 [0.666, 0.668] |
| ref-T / ref | 256 | prepared | 8 | 31,120 | 26,879 | 0.864 [0.863, 0.865] |
| ref-T / ref | 512 | all-in | 8 | 17,892 | 10,696 | 0.598 [0.597, 0.598] |
| ref-T / ref | 512 | prepared | 8 | 17,694 | 13,547 | 0.766 [0.765, 0.766] |
| ref-T / ref | 1024 | all-in | 8 | 8,540 | 5,127 | 0.600 [0.600, 0.601] |
| ref-T / ref | 1024 | prepared | 8 | 8,887 | 6,653 | 0.749 [0.748, 0.749] |
| hot64 / hot64 | 256 | all-in | 8 | 34,892 | 25,661 | 0.735 [0.735, 0.736] |
| hot64 / hot64 | 256 | prepared | 8 | 33,864 | 34,139 | 1.008 [1.007, 1.009] |
| hot64 / hot64 | 512 | all-in | 8 | 19,243 | 12,833 | 0.667 [0.666, 0.667] |
| hot64 / hot64 | 512 | prepared | 8 | 19,014 | 17,170 | 0.903 [0.902, 0.903] |
| hot64 / hot64 | 1024 | all-in | 8 | 9,160 | 6,124 | 0.669 [0.668, 0.669] |
| hot64 / hot64 | 1024 | prepared | 8 | 9,558 | 8,432 | 0.882 [0.882, 0.883] |
| hot32 / hot32 | 256 | all-in | 8 | 44,107 | 29,825 | 0.676 [0.676, 0.677] |
| hot32 / hot32 | 256 | prepared | 8 | 42,675 | 41,927 | 0.982 [0.982, 0.983] |
| hot32 / hot32 | 512 | all-in | 8 | 23,871 | 14,913 | 0.625 [0.624, 0.625] |
| hot32 / hot32 | 512 | prepared | 8 | 23,518 | 21,111 | 0.898 [0.897, 0.898] |
| hot32 / hot32 | 1024 | all-in | 8 | 11,209 | 7,064 | 0.630 [0.630, 0.630] |
| hot32 / hot32 | 1024 | prepared | 8 | 11,810 | 10,325 | 0.874 [0.874, 0.875] |

## Table 6: Stage breakdown at the headline variant (ref, one thread, all-in) (flags O3native, run set headline)

Profile benches (hawk_sign_profile_bench: upstream HAWK ref; e8_sign_profile_bench: E8 ref): TSC cycles per accepted signature including the per-stage timers, so the totals exceed the timed figures; mean over runs.

| scheme | n | stage | cycles / sign | share | runs |
| --- | ---: | --- | ---: | ---: | ---: |
| HAWK | 256 | key expand | 6,454 | 5.1% | 5 |
| HAWK | 256 | hash msg hpub | 1,357 | 1.1% | 5 |
| HAWK | 256 | salt derivation | 1,860 | 1.5% | 5 |
| HAWK | 256 | challenge hash | 1,803 | 1.4% | 5 |
| HAWK | 256 | target coset | 2,796 | 2.2% | 5 |
| HAWK | 256 | gaussian sampler | 75,606 | 60.2% | 5 |
| HAWK | 256 | norm rejection | 308 | 0.2% | 5 |
| HAWK | 256 | reconstruct s1 | 29,688 | 23.6% | 5 |
| HAWK | 256 | symbreak bounds | 1,926 | 1.5% | 5 |
| HAWK | 256 | encode sig | 2,510 | 2.0% | 5 |
| HAWK | 256 | other | 1,227 | 1.0% | 5 |
| HAWK | 256 | total | 125,535 | 100.0% | 5 |
| HAWK | 512 | key expand | 14,722 | 6.6% | 5 |
| HAWK | 512 | hash msg hpub | 1,392 | 0.6% | 5 |
| HAWK | 512 | salt derivation | 1,661 | 0.7% | 5 |
| HAWK | 512 | challenge hash | 1,625 | 0.7% | 5 |
| HAWK | 512 | target coset | 6,118 | 2.8% | 5 |
| HAWK | 512 | gaussian sampler | 125,219 | 56.5% | 5 |
| HAWK | 512 | norm rejection | 253 | 0.1% | 5 |
| HAWK | 512 | reconstruct s1 | 59,407 | 26.8% | 5 |
| HAWK | 512 | symbreak bounds | 3,553 | 1.6% | 5 |
| HAWK | 512 | encode sig | 6,510 | 2.9% | 5 |
| HAWK | 512 | other | 1,126 | 0.5% | 5 |
| HAWK | 512 | total | 221,586 | 100.0% | 5 |
| HAWK | 1024 | key expand | 52,964 | 11.3% | 5 |
| HAWK | 1024 | hash msg hpub | 1,481 | 0.3% | 5 |
| HAWK | 1024 | salt derivation | 2,842 | 0.6% | 5 |
| HAWK | 1024 | challenge hash | 2,864 | 0.6% | 5 |
| HAWK | 1024 | target coset | 17,330 | 3.7% | 5 |
| HAWK | 1024 | gaussian sampler | 253,480 | 53.9% | 5 |
| HAWK | 1024 | norm rejection | 253 | 0.1% | 5 |
| HAWK | 1024 | reconstruct s1 | 117,933 | 25.1% | 5 |
| HAWK | 1024 | symbreak bounds | 6,792 | 1.4% | 5 |
| HAWK | 1024 | encode sig | 12,807 | 2.7% | 5 |
| HAWK | 1024 | other | 1,136 | 0.2% | 5 |
| HAWK | 1024 | total | 469,882 | 100.0% | 5 |
| E8 | 256 | key prepare | 50,275 | 24.5% | 5 |
| E8 | 256 | salt | 2,718 | 1.3% | 5 |
| E8 | 256 | hash challenge | 4,142 | 2.0% | 5 |
| E8 | 256 | target coset | 6,972 | 3.4% | 5 |
| E8 | 256 | e8 sampler | 105,019 | 51.2% | 5 |
| E8 | 256 | reconstruct s | 33,632 | 16.4% | 5 |
| E8 | 256 | norm rejection | 511 | 0.2% | 5 |
| E8 | 256 | encode sig | 289 | 0.1% | 5 |
| E8 | 256 | other | 1,446 | 0.7% | 5 |
| E8 | 256 | total | 205,005 | 100.0% | 5 |
| E8 | 512 | key prepare | 103,688 | 25.5% | 5 |
| E8 | 512 | salt | 2,742 | 0.7% | 5 |
| E8 | 512 | hash challenge | 5,267 | 1.3% | 5 |
| E8 | 512 | target coset | 16,439 | 4.1% | 5 |
| E8 | 512 | e8 sampler | 207,270 | 51.1% | 5 |
| E8 | 512 | reconstruct s | 68,106 | 16.8% | 5 |
| E8 | 512 | norm rejection | 513 | 0.1% | 5 |
| E8 | 512 | encode sig | 323 | 0.1% | 5 |
| E8 | 512 | other | 1,482 | 0.4% | 5 |
| E8 | 512 | total | 405,830 | 100.0% | 5 |
| E8 | 1024 | key prepare | 229,596 | 27.1% | 5 |
| E8 | 1024 | salt | 3,926 | 0.5% | 5 |
| E8 | 1024 | hash challenge | 8,627 | 1.0% | 5 |
| E8 | 1024 | target coset | 45,007 | 5.3% | 5 |
| E8 | 1024 | e8 sampler | 417,451 | 49.3% | 5 |
| E8 | 1024 | reconstruct s | 139,394 | 16.5% | 5 |
| E8 | 1024 | norm rejection | 512 | 0.1% | 5 |
| E8 | 1024 | encode sig | 417 | 0.0% | 5 |
| E8 | 1024 | other | 1,562 | 0.2% | 5 |
| E8 | 1024 | total | 846,491 | 100.0% | 5 |

## Table 7: Context: HAWK AVX2 (upstream Optimized_Implementation; not a fair pair) (flags O3native, run set headline)

Implementation context only: E8 has no AVX2 path, so no claim about the lattice rests on this table. Primary cycles, one thread, same harness and flag set.

| n | acct | HAWK ref | HAWK ref-avx2 | AVX2 / ref | E8 ref | E8 ref / HAWK AVX2 [95% CI] |
| ---: | --- | ---: | ---: | ---: | ---: | --- |
| 256 | all-in | 109,956 | 29,179 | 0.265 | 200,999 | 6.888 [6.831, 6.923] |
| 256 | prepared | 112,839 | 37,839 | 0.335 | 159,540 | 4.216 [4.189, 4.235] |
| 512 | all-in | 219,704 | 51,283 | 0.233 | 401,158 | 7.822 [7.755, 7.863] |
| 512 | prepared | 223,336 | 68,654 | 0.307 | 316,548 | 4.611 [4.581, 4.629] |
| 1024 | all-in | 468,794 | 107,857 | 0.230 | 836,791 | 7.758 [7.694, 7.797] |
| 1024 | prepared | 445,186 | 130,192 | 0.292 | 644,580 | 4.951 [4.918, 4.970] |

## Table 8: Key generation: cycles per key (flags O3native, run set headline)

Each scheme's public keygen call (RNG to encoded private and public key, retries included), one thread, pinned, interleaved per key index in rotated order after warm-up. HAWK, E8 and the core (Hawk_keygen() alone: HAWK's NTRU core, which both call) draw from the same paired seed per key index. mean: mean of the per-run means over key indices (key generation restarts, so its expected cost is the mean); median: mean of the per-run medians. E8/HAWK: ratio of means, 95% CI from a paired bootstrap (runs, then key indices). tail: the scheme's mean minus the core's on the same key indices (HAWK: public-key encoding; E8: its public form, key generation limits and invertibility check over 3 primes; HAWK's own form and range check run inside the core for both). diverged: share of key indices where HAWK and E8 made different numbers of RNG calls (one restarted after the core). draws: (f, g) draws (RNG calls) per key. Key bytes from sizes.csv. HAWK AVX2 rows (ref-avx2): context, not a fair pair (E8 / HAWK AVX2 in its ratio column).

| n | scheme | variant | mean | median | E8/HAWK [95% CI] | tail | diverged | draws | pub B | priv B | runs x keys |
| ---: | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | HAWK | ref | 2,406,411 | 2,219,728 |  | 23,313 |  | 73.1 | 450 | 96 | 5 x 1000 |
| 256 | E8 | ref | 2,457,246 | 2,283,010 | 1.021 [1.019, 1.023] | 74,148 | 0.5% | 72.6 | 2,048 | 544 | 5 x 1000 |
| 256 | core | ref | 2,383,098 | 2,208,323 |  |  |  | 72.6 |  |  | 5 x 1000 |
| 256 | HAWK | ref-avx2 | 1,209,166 | 1,137,991 | 2.032 [2.025, 2.039] | 16,011 |  | 73.1 | 450 | 96 | 5 x 1000 |
| 256 | core | ref-avx2 | 1,193,156 | 1,128,244 |  |  |  | 72.6 |  |  | 5 x 1000 |
| 512 | HAWK | ref | 8,543,438 | 7,164,870 |  | 723,198 |  | 18.7 | 1,024 | 184 | 5 x 1000 |
| 512 | E8 | ref | 7,969,988 | 7,262,561 | 0.933 [0.926, 0.940] | 149,747 | 8.5% | 17.3 | 4,096 | 1,080 | 5 x 1000 |
| 512 | core | ref | 7,820,240 | 7,112,488 |  |  |  | 17.3 |  |  | 5 x 1000 |
| 512 | HAWK | ref-avx2 | 5,382,408 | 4,342,294 | 1.481 [1.468, 1.492] | 459,408 |  | 18.7 | 1,024 | 184 | 5 x 1000 |
| 512 | core | ref-avx2 | 4,923,000 | 4,311,585 |  |  |  | 17.3 |  |  | 5 x 1000 |
| 1024 | HAWK | ref | 47,136,616 | 44,066,315 |  | 819,266 |  | 10.6 | 2,440 | 360 | 5 x 1000 |
| 1024 | E8 | ref | 46,615,889 | 38,035,013 | 0.989 [0.985, 0.993] | 298,539 | 1.6% | 10.4 | 8,192 | 2,152 | 5 x 1000 |
| 1024 | core | ref | 46,317,350 | 37,742,615 |  |  |  | 10.4 |  |  | 5 x 1000 |
| 1024 | HAWK | ref-avx2 | 34,081,566 | 32,410,611 | 1.368 [1.361, 1.375] | 606,236 |  | 10.6 | 2,440 | 360 | 5 x 1000 |
| 1024 | core | ref-avx2 | 33,475,329 | 25,818,440 |  |  |  | 10.4 |  |  | 5 x 1000 |

## Table 9: Verification: cycles per signature (flags O3native, run set headline)

One thread, pinned, interleaved per signature in rotated order after warm-up; inputs: the paired-stream keys, each scheme's ref signer, all-in, T = 1 (verification cost does not depend on the sampler). all-in: encoded signature and public key (decoding and hashing inside the timed call; HAWK with HAWK_TMPSIZE_VERIFY_FAST, E8 with E8_TMPSIZE_VERIFY); prepared: the public key decoded once per key outside the timed call (decode only, for both; timed as pub dec), the signature still encoded. Estimate: median per run, mean over runs; E8/HAWK with its bootstrap 95% CI. primes: NTT primes of the norm check, each scheme the minimum its own HAWK-style range argument proves sound: HAWK 2, E8 3, because the E8-CM public form (paper Sec. 4.5) has coefficients 2^3 to 2^7 times larger than HAWK's (e8/docs/verifier_range.md); a cost of the lattice. E8 fast has HAWK's verifier techniques (precomputed twiddles, constant NTT of Delta, batch inversion, integer bound, early reject). E8 ref (4 primes, twiddles and Delta rebuilt per call, one Fermat inversion per coefficient, double bound) is the verifier the submission measured, as context. HAWK reconstructs s0 from s1 (its compressed signature carries s1 only); E8's uncompressed signature carries s0 (about 4 times larger), so E8 does not. ref-avx2: HAWK AVX2 context (E8 fast / HAWK AVX2 in its ratio column).

| n | acct | scheme | variant | verifier | primes | cycles | TSC | E8/HAWK [95% CI] | pub dec | sig B | pub B |
| ---: | --- | --- | --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| 256 | all-in | HAWK | ref | upstream | 2 | 87,088 | 86,542 |  | 11,509 | 249 | 450 |
| 256 | all-in | E8 | ref | fast | 3 | 119,147 | 118,474 | 1.368 [1.367, 1.371] | 21,983 | 1,038 | 2,048 |
| 256 | all-in | E8 | ref | ref | 4 | 708,700 | 706,653 | 8.138 [8.124, 8.159] | 21,983 | 1,038 | 2,048 |
| 256 | all-in | HAWK | ref-avx2 | upstream | 2 | 39,543 | 39,121 | 3.013 [3.008, 3.020] | 7,819 | 249 | 450 |
| 256 | prepared | HAWK | ref | upstream | 2 | 78,279 | 77,735 |  | 11,509 | 249 | 450 |
| 256 | prepared | E8 | ref | fast | 3 | 98,544 | 97,945 | 1.259 [1.257, 1.262] | 21,983 | 1,038 | 2,048 |
| 256 | prepared | E8 | ref | ref | 4 | 688,336 | 686,392 | 8.793 [8.778, 8.817] | 21,983 | 1,038 | 2,048 |
| 256 | prepared | HAWK | ref-avx2 | upstream | 2 | 32,725 | 32,327 | 3.011 [3.004, 3.021] | 7,819 | 249 | 450 |
| 512 | all-in | HAWK | ref | upstream | 2 | 175,010 | 174,210 |  | 23,877 | 555 | 1,024 |
| 512 | all-in | E8 | ref | fast | 3 | 234,855 | 233,894 | 1.342 [1.341, 1.343] | 41,747 | 2,072 | 4,096 |
| 512 | all-in | E8 | ref | ref | 4 | 1,421,238 | 1,422,229 | 8.121 [8.119, 8.124] | 41,747 | 2,072 | 4,096 |
| 512 | all-in | HAWK | ref-avx2 | upstream | 2 | 76,802 | 76,291 | 3.058 [3.055, 3.062] | 14,907 | 555 | 1,024 |
| 512 | prepared | HAWK | ref | upstream | 2 | 157,043 | 156,271 |  | 23,877 | 555 | 1,024 |
| 512 | prepared | E8 | ref | fast | 3 | 194,755 | 193,904 | 1.240 [1.239, 1.242] | 41,747 | 2,072 | 4,096 |
| 512 | prepared | E8 | ref | ref | 4 | 1,382,053 | 1,383,473 | 8.800 [8.797, 8.804] | 41,747 | 2,072 | 4,096 |
| 512 | prepared | HAWK | ref-avx2 | upstream | 2 | 62,701 | 62,221 | 3.106 [3.102, 3.111] | 14,907 | 555 | 1,024 |
| 1024 | all-in | HAWK | ref | upstream | 2 | 356,259 | 354,952 |  | 53,545 | 1,221 | 2,440 |
| 1024 | all-in | E8 | ref | fast | 3 | 473,238 | 471,615 | 1.328 [1.327, 1.329] | 79,663 | 4,136 | 8,192 |
| 1024 | all-in | E8 | ref | ref | 4 | 2,875,506 | 2,873,230 | 8.071 [8.070, 8.073] | 79,663 | 4,136 | 8,192 |
| 1024 | all-in | HAWK | ref-avx2 | upstream | 2 | 157,738 | 156,950 | 3.000 [2.999, 3.002] | 31,837 | 1,221 | 2,440 |
| 1024 | prepared | HAWK | ref | upstream | 2 | 312,471 | 311,311 |  | 53,545 | 1,221 | 2,440 |
| 1024 | prepared | E8 | ref | fast | 3 | 394,032 | 392,624 | 1.261 [1.260, 1.262] | 79,663 | 4,136 | 8,192 |
| 1024 | prepared | E8 | ref | ref | 4 | 2,797,859 | 2,795,417 | 8.954 [8.952, 8.956] | 79,663 | 4,136 | 8,192 |
| 1024 | prepared | HAWK | ref-avx2 | upstream | 2 | 126,854 | 126,171 | 3.106 [3.104, 3.109] | 31,837 | 1,221 | 2,440 |

## Table 10: Sizes in bytes

From the size macros of hawk.h and hawk_e8.h (sizes.csv). E8's prepared private key holds the per-key NTT and F2 basis tables of the signer in a fixed 40,960-byte layout; its decoded public key holds q00 and q01 as int32 (no precomputed NTT, as HAWK's). The E8 functions also use stack buffers beyond tmp (not counted). Empty: the object does not exist for the scheme.

| object | HAWK 256 | E8 256 | HAWK 512 | E8 512 | HAWK 1024 | E8 1024 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| private key (encoded) | 96 | 544 | 184 | 1,080 | 360 | 2,152 |
| private key (prepared / decoded) | 592 | 40,960 | 1,184 | 40,960 | 2,368 | 40,960 |
| public key (encoded) | 450 | 2,048 | 1,024 | 4,096 | 2,440 | 8,192 |
| public key (decoded) | 784 | 2,064 | 1,568 | 4,128 | 3,136 | 8,256 |
| signature (encoded) | 249 | 1,038 | 555 | 2,072 | 1,221 | 4,136 |
| signature (decoded) | 526 |  | 1,048 |  | 2,088 |  |
| tmp: key generation | 5,639 | 6,151 | 11,271 | 12,295 | 22,535 | 24,583 |
| tmp: signing (encoded key) | 1,543 | 40,960 | 3,079 | 40,960 | 6,151 | 40,960 |
| tmp: verification | 2,567 | 2,055 | 5,127 | 4,103 | 10,247 | 8,199 |
| tmp: verification, fastest | 3,911 |  | 7,751 |  | 15,495 |  |

