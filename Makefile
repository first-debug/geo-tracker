CXX:= g++

NVDS_VERSION:=9.0
LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/

APP:= sheremetevo-app
BUILD_DIR:= build
CONFIGS_DIR:= configs
GRAPHS_DIR:= graphs
GEN_DIR:= $(BUILD_DIR)/gen

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

.PHONY: all run install clean

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

gen-graph: $(TARGET) | $(GRAPHS_DIR)
	GST_DEBUG_DUMP_DOT_DIR=graphs $(MAKE) run-multi-src

graph:
	dot -Tpng graphs/sheremetevo.dot -o graphs/sheremetevo.png

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GRAPHS_DIR)
