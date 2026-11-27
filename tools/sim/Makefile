# Desktop render of ui.cpp (LVGL -> PPM images). Run from tools/:
#   ./fetch_samples.sh && make -C sim -j8 && mkdir -p sim/out && (cd . && sim/sim)
# Output: tools/out/*.ppm  (convert with: sips -s format png out/*.ppm --out out/)
P=../..
CF=-O1 -I stub -I $(P) -I $(P)/lib -I $(P)/lib/lvgl -I $(P)/lib/ArduinoJson/src -DLV_CONF_INCLUDE_SIMPLE -w
SRCS=$(shell find $(P)/lib/lvgl/src -name '*.c')
OBJS=$(patsubst $(P)/lib/lvgl/src/%.c,obj/%.o,$(SRCS))
sim: $(OBJS) sim.cc $(P)/ui.cpp $(P)/f1_model.cpp
	c++ -std=c++17 $(CF) sim.cc $(P)/ui.cpp $(P)/f1_model.cpp $(OBJS) -o sim
obj/%.o: $(P)/lib/lvgl/src/%.c
	@mkdir -p $(dir $@)
	@cc $(CF) -c $< -o $@
