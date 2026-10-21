CXX:= g++

NVDS_VERSION:=9.0
LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/

APP:= sheremetevo-app
BUILD_DIR:= build
CONFIGS_DIR:= configs
GRAPHS_DIR:= graphs
GEN_DIR:= $(BUILD_DIR)/gen
NVINFER_CONFIG?= $(CONFIGS_DIR)/config_infer_primary_yolo.txt

TARGET:= $(BUILD_DIR)/$(APP)

SRCS+= $(wildcard *.cpp)
INCS+= $(wildcard *.hpp)
OBJS+= $(SRCS:.cpp=.o)
PKGS:= gstreamer-1.0 opencv4 protobuf

# Код protobuf генерируется из proto/*.proto в build/gen
PROTOS:= $(wildcard proto/*.proto)
PROTO_SRCS:= $(patsubst proto/%.proto,$(GEN_DIR)/%.pb.cc,$(PROTOS))
PROTO_HDRS:= $(PROTO_SRCS:.cc=.h)
PROTO_OBJS:= $(PROTO_SRCS:.cc=.o)

BUILD_OBJS:= $(addprefix $(BUILD_DIR)/, $(OBJS))

CFLAGS+= -std=gnu++17 -O2 -g -Wall -Wextra \
		 $(shell pkg-config --cflags $(PKGS)) \
		 -I/opt/nvidia/deepstream/deepstream/sources/includes \
		 -I/usr/include/opencv4 \
		 -I$(GEN_DIR)

LIBS:= $(shell pkg-config --libs $(PKGS)) \
		-L$(LIB_INSTALL_DIR) \
		-lnvdsgst_meta \
		-lnvds_meta \
		-lnvds_yml_parser \
		-lcuda \
		-lnvbufsurface \
		-Wl,-rpath,$(LIB_INSTALL_DIR) \
		-pthread

.PHONY: all run install clean test engine engine-rebuild

all: $(TARGET)

run: all
	$(MAKE) run-multi-src

run-single-src: $(TARGET)
	$(TARGET) $(CONFIGS_DIR)/app.ini

run-multi-src: $(TARGET)
	$(TARGET) $(CONFIGS_DIR)/app.ini

$(BUILD_DIR):
	mkdir -p $@

$(GEN_DIR):
	mkdir -p $@

$(GRAPHS_DIR):
	mkdir -p $@

$(MEDIA_DIR):
	mkdir -p $@

$(GEN_DIR)/%.pb.cc $(GEN_DIR)/%.pb.h: proto/%.proto | $(GEN_DIR)
	protoc -Iproto --cpp_out=$(GEN_DIR) $<

# иначе make считает сгенерированные файлы промежуточными и удаляет их
.SECONDARY: $(PROTO_SRCS) $(PROTO_HDRS)

$(GEN_DIR)/%.pb.o: $(GEN_DIR)/%.pb.cc $(GEN_DIR)/%.pb.h
	$(CXX) -c -o $@ $(CFLAGS) $<

$(BUILD_DIR)/%.o: %.cpp $(INCS) $(PROTO_HDRS) Makefile | $(BUILD_DIR)
	$(CXX) -c -o $@ $(CFLAGS) $<

$(TARGET): $(BUILD_OBJS) $(PROTO_OBJS) Makefile | $(BUILD_DIR)
	$(CXX) -o $@ $(BUILD_OBJS) $(PROTO_OBJS) $(LIBS)

# TensorRT engine по model-engine-file из NVINFER_CONFIG. Строится, только если
# его нет или поменялись ONNX, GPU, TensorRT или параметры сборки (нужен GPU).
engine:
	bash tools/build_engine.sh $(NVINFER_CONFIG)

engine-rebuild:
	bash tools/build_engine.sh --force $(NVINFER_CONFIG)

gen-graph: $(TARGET) | $(GRAPHS_DIR)
	GST_DEBUG_DUMP_DOT_DIR=graphs $(MAKE) run-multi-src

graph:
	dot -Tpng graphs/sheremetevo.dot -o graphs/sheremetevo.png

# Юнит-тесты: собираются без DeepStream, GStreamer и GPU (нужны только
# glib, OpenCV и protobuf), поэтому запускаются где угодно, в том числе в CI.
#   make test                    — собрать и запустить все тесты
#   make test TEST_FILTER=csv    — только тесты, в имени которых есть подстрока
TEST_DIR:= $(BUILD_DIR)/tests
TEST_BIN:= $(TEST_DIR)/run-tests
TEST_PKGS:= glib-2.0 opencv4 protobuf
# код приложения, который покрывают тесты
TEST_APP_SRCS:= app_config.cpp pixel_transformer.cpp points_sender.cpp udp_connection.cpp
TEST_SRCS:= $(wildcard tests/*.cpp)
TEST_OBJS:= $(addprefix $(TEST_DIR)/, $(notdir $(TEST_APP_SRCS:.cpp=.o) $(TEST_SRCS:.cpp=.o))) \
			$(TEST_DIR)/points.pb.o
TEST_CFLAGS:= -std=gnu++17 -O1 -g -Wall -Wextra \
			  $(shell pkg-config --cflags $(TEST_PKGS)) \
			  -I$(GEN_DIR) \
			  -DTEST_SRC_DIR='"$(CURDIR)"'
TEST_LIBS:= $(shell pkg-config --libs $(TEST_PKGS)) -pthread

test: $(TEST_BIN)
	$(TEST_BIN) $(TEST_FILTER)

$(TEST_DIR):
	mkdir -p $@

$(TEST_DIR)/%.o: %.cpp $(INCS) $(PROTO_HDRS) Makefile | $(TEST_DIR)
	$(CXX) -c -o $@ $(TEST_CFLAGS) $<

$(TEST_DIR)/%.o: tests/%.cpp tests/test.hpp $(INCS) $(PROTO_HDRS) Makefile | $(TEST_DIR)
	$(CXX) -c -o $@ $(TEST_CFLAGS) $<

$(TEST_DIR)/%.pb.o: $(GEN_DIR)/%.pb.cc $(GEN_DIR)/%.pb.h | $(TEST_DIR)
	$(CXX) -c -o $@ $(TEST_CFLAGS) $<

$(TEST_BIN): $(TEST_OBJS) Makefile
	$(CXX) -o $@ $(TEST_OBJS) $(TEST_LIBS)

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GRAPHS_DIR)
