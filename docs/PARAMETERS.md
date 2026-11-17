# Recommended values for the open numbers (to confirm at the meeting)

| Item | Recommendation | Why |
|------|----------------|-----|
| Time-bucket width | **15 min** (96 buckets), linear interpolation; **switchable**: `make_profile.py --buckets 288` gives 5 min, no code change (read from `profile.csv`, tested at both) | the profile is synthetic, so finer buckets add resolution to an invented curve; the choice is a judgement call, not a spec requirement |
| Departure-time rule | **4 fixed departures** 03:00, 08:00, 12:30, 17:30, rotated inside every bin (equal share); optional extra run with seeded random departures as a sensitivity check | departure time changes query difficulty (peak = looser heuristic), so each bin gets the same mix and results can be reported per departure; seeded random would also be reproducible |
| Distance bins | 1-3, 3-8, 8-15, 15-25, >25 km (straight line). Drop/merge any bin the pilot on real HCMC shows below the 20 ms batch floor | short queries measure the timer |
| Pairs per bin | **50** (scalar runs and golden file); Pareto oracle on **10** per bin (cost grows with front size) | statistics vs oracle runtime |
| Repetitions | **10 + 1 warm-up**, report median and IQR | A1 protocol |
| Run budget per platform | **<= 5 h** measured wall-clock; thread counts {1, 2, 4, physical cores, logical cores}; `tools/pilot.py` projects it before you start | leaves a day for reruns |
| Vector quality metric | **Hypervolume ratio** (ref point 1.1 x max of exact front), plus exact-point recall and front-size ratio | one number per query + two diagnostics |
| Noise range for uncertain h | rho in **{0, 100, 250, 500} permille**, weight w in **{100, 120, 150} %**; headline config rho=250, w=100; 3 noise seeds for rho>0 | 0 = control; 25 % is a realistic estimate error; 50 % is the stress case |
| Objectives in experiments | time (primary), distance (secondary), vector (Pareto) | spec asks for vector f |
