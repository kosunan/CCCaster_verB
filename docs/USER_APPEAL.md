# CCCaster verB 1.0 beta — Faster startup. More consistent frame pacing.

For MBAACC Ver.1.07 Rev.1.4.0 (32-bit). Updated September 13, 2026.

**Get into training faster, keep both players on shared netplay settings, and recover efficiently when inputs need correcting.**

## Highlights

- **About 76% faster training startup:** approximately **8.71 seconds → 2.07 seconds** in our same-PC comparison with the legacy build.
- **More consistent offline frame pacing:** after the latest update, **99.7% of measured frame intervals were within 3µs of 1/60 second**. The 99th-percentile absolute error fell from approximately **354µs to 1.93µs** compared with the previous v10 pacing path.
- **Shared delay and rollback settings:** both players use the agreed settings, with coordinated match starts and ongoing clock adjustment to reduce differences in progression.
- **A dedicated GUI:** host, join, spectate, or enter training through English and Japanese menus. Configure controllers in-game with F4.
- **Simpler connection sharing:** one connection code carries address and port information, including IPv4 and IPv6 candidates.
- **Efficient rollback recovery:** skip intermediate presentation and game waits while recalculating, then return to normal rendering when caught up.
- **Independent input capture:** collect and exchange frame-numbered inputs through a central buffer while the game handles prediction and correction.

Performance figures are from our Ryzen 7 5800X3D test PC. Startup uses three runs per version; the latest pacing comparison uses two completed training runs per configuration, with 1,500 intervals per run. These are measured results, not a guarantee for every PC or frame. [Startup comparison](benchmarks/2026-09-13_legacy_comparison.md) · [Latest pacing results](benchmarks/2026-09-13_offline_pacing.md)

## Start playing sooner

Training startup took a median of **8.713 seconds with the legacy build and 2.069 seconds with v10**, saving approximately **6.64 seconds**. All three measured v10 launches finished in under 2.08 seconds.

Startup work has been reduced across graphics initialization, loading transitions, texture preparation, and controller discovery. The launcher transfers 296 DDS textures in their existing compressed format and prepares assets alongside controller enumeration.

The measurement runs from the launcher request to frame progression on the character-select screen. [Conditions and all startup runs](benchmarks/2026-09-13_legacy_comparison.md)

## More consistent frame pacing

One frame at 60Hz lasts **16,666.666…µs**. v10 schedules normal updates against absolute deadlines, including processing, drawing, and waiting within the frame budget.

The latest update improves the offline training path: high-resolution waiting replaces coarse sleep calls, and input preparation finishes before the final release deadline. The game then waits at the final release point before continuing.

| Absolute frame-interval error | Previous v10 pacing | Updated v10 pacing |
|---|---:|---:|
| Median | 1.63µs | **0.83µs** |
| 95th percentile | 16.83µs | **1.63µs** |
| 99th percentile | 353.55µs | **1.93µs** |
| Intervals within 3µs | 66.5% | **99.7%** |

These figures cover **3,000 intervals per configuration**, measured at the same point in the game with a shared external QPC observer. Rare outliers remain: the updated runs had a maximum absolute error of approximately 450µs. [Full results and methodology](benchmarks/2026-09-13_offline_pacing.md)

The clock remains based on a continuously fed WASAPI audio stream, with QPC interpolation for short waits. Fractional timing is retained internally rather than repeatedly rounding each frame to a whole microsecond.

## Keep both players on shared settings

v10 applies **agreed input delay and rollback limits to both players**. The default is **D2/R4**, with D ≥ 0, R ≥ 0, and D + R ≤ 8.

It also coordinates a future match-start time and adjusts for estimated clock differences during play. Together, shared settings and timing coordination reduce factors that can make one player run further ahead and require more prediction.

A previous 120-second v10 test recorded the estimated clock phase difference converging from **533.48µs to 8.55µs**, with matching confirmed inputs and checked game state across **5,737 frames**. [Clock synchronization measurements](design/2026-09-12_clock_follow.md)

## Recover efficiently from corrected inputs

When a received input differs from the prediction used for that frame, v10 restores the relevant state and recalculates using the buffered input history.

During recalculation, API hooks suppress intermediate presentation, HUD rendering, and game waits. Normal rendering resumes after recovery. Redundant drawing work and unnecessary saving of already-confirmed frames have also been reduced.

Existing v10 measurements include **1,063 internal replay updates in approximately 0.348 seconds** during spectator catch-up, and a separate snapshot optimization that reduced median rollback recalculation time, including restoration, from **874.5µs to 758.15µs**. These are separate workloads, not an old-versus-new FPS benchmark. [Spectator catch-up](design/2026-09-12_spectator_stream.md) · [Snapshot optimization](design/2026-09-11_confirmed_replay_snapshots.md)

## Capture inputs independently

An independent input clock captures controller states, tags them with frame numbers and generations, and stores them in a central buffer. Capture and transmission can continue while the game recalculates earlier frames.

Received inputs are matched to the corresponding frames. Acknowledgments and retransmission handle missing or reordered packets. Game-state writes, saves, and restores remain on the game thread.

## A clearer way to connect and play

The GUI brings hosting, joining, spectating, training, connection progress, and cancellation into one interface, with English and Japanese support.

Share a connection code instead of separately typing multiple addresses and ports. Connection codes can carry IPv4, IPv6, and local address candidates. Spectators join using an S-code.

Press **F4** to review or change controller mappings. The setup screen checks duplicate assignments and missing required inputs, supports cancellation and defaults, and provides training-only state save/load assignments.

## Short announcement

> **CCCaster verB 1.0 beta: faster startup, more consistent frame pacing, and streamlined netplay.**
>
> Start training in about **2.1 seconds**, down from **8.7 seconds** in our legacy-build comparison. The latest offline timing update kept **99.7% of measured frame intervals within 3µs of 1/60 second** on our test PC.
>
> Shared delay and rollback settings, coordinated clocks, independent input capture, and efficient rollback recovery support netplay. An English/Japanese GUI and shareable connection codes bring hosting, joining, spectating, and training together.
>
> Results reflect the documented test conditions; performance varies by system. See the linked benchmarks for details.
