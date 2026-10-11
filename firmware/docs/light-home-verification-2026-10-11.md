# Light home verification, 2026-10-11

Firmware built successfully and flashed to COM4, MAC 98:c3:77:f5:52:70; esptool hash verified. Final firmware size 2506720 bytes.

Implemented: Codex render buffers and project catalog allocated on entry (8800 bytes) and freed on exit; home bridge does not auto-request project catalog. Radio full control tree cleared on exit; same audio core continues at home. Catalog discovery deferred to full radio, existing cached catalog skips automatic refresh. National fallback results do not overwrite an existing cached catalog. MP3 prewarmed pool retained because its decoder requires a contiguous block unavailable after Wi-Fi startup.

Evidence:
- radio_light_lifecycle.log: Chongqing city search returned 2 entries; merged 11 stations; jazz, retro and Chongqing urban radio emitted PCM. Home return retained uninterrupted PCM, 10-second stable windows pcm=10004ms, gaps30=0, rebuffers=0.
- light_home_restart.log: restored 11 cached stations; home playback emitted PCM; completion event accepted without project request.
- light_codex_lifecycle.log: full Codex entry free heap ~20KB; exit to home ~36KB; failure notification configured audio output, no panic/watchdog.
- radio_restore_city.log: manually restored Chongqing catalog after a failed directory query replaced previous cache during testing; 11 stations persisted. Two stable windows pcm=10004ms, gap_max=12ms, rebuffers=0.
- radio_light_final.log: final firmware restart restored 11 stations; full page entry did not start discovery; Chongqing urban radio emitted PCM and stable windows.

Tests: coding workspace lifecycle, radio retry, stream buffer, MP3 header all exited 0. ESP-IDF build exited 0; existing deprecation/unused-function warnings remain.

Limitations: serial PCM evidence does not prove acoustic quality; all external station endpoints have not been exhaustively tested. Initial stream connection/prebuffer can take approximately 5 seconds. Directory service intermittently times out; valid cache is preserved by final firmware.

Rulings: Work in the supplied directory because it is not a git checkout. apply_patch calls aborted, so edits were applied with structured replacements. Preserve decoder pool rather than releasing it at home because reliable reallocation fails after Wi-Fi fragments heap. Do not claim full minimal Codex core: task/result protocol buffers remain resident; large UI render/catalog workspace is now lazy.
