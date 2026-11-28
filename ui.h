// LVGL screens: the live screens (tower, map, sectors, tyres, one car),
// alerts, and the between-session screens. Nothing changes screen on its
// own: only a tap does.
#pragma once
void uiBegin();   // call with the LVGL port lock held
// Bench aid (serial console): "V1" = live screen 1 (0 tower, 1 map, 2 sectors,
// 3 tyres, 4 car), "D44" = car 44's screen, "I1" = between-session screen 1, "A3" =
// show alert 3 (F1AlertKind: 1 start .. 8 chequered, 9 overtake, 10 pit,
// 11 radio, 12 fastest lap) as if P1 and P2 were involved.
void uiDebug(const char *cmd);
