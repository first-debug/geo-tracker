# Сборка sheremetevo. Список целей: make help
#
# Приложение собирается только в DeepStream-контейнере (docs/deploy.md).
# Юнит-тесты (make test) не требуют DeepStream, GStreamer и GPU.

.DEFAULT_GOAL:= all

########################################################################
# Настройки (любую можно переопределить: make VAR=значение)
########################################################################

CXX:= g++
DS_ROOT?= /opt/nvidia/deepstream/deepstream
NVINFER_CONFIG?= configs/config_infer_primary_yolo.txt
RUN_CONFIG?= configs/app.ini
TEST_FILTER?=

BUILD_DIR:= build
GEN_DIR:= $(BUILD_DIR)/gen
TEST_DIR:= $(BUILD_DIR)/tests
GRAPHS_DIR:= graphs

TARGET:= $(BUILD_DIR)/sheremetevo-app
TEST_BIN:= $(TEST_DIR)/run-tests

COMMON_CXXFLAGS:= -std=gnu++17 -g -Wall -Wextra -MMD -MP -I$(GEN_DIR)

APP_PKGS:= gstreamer-1.0 opencv4 protobuf
APP_CXXFLAGS= $(COMMON_CXXFLAGS) -O2 \
              $(shell pkg-config --cflags $(APP_PKGS)) \
              -I$(DS_ROOT)/sources/includes
APP_LIBS= $(shell pkg-config --libs $(APP_PKGS)) \
          -L$(DS_ROOT)/lib -Wl,-rpath,$(DS_ROOT)/lib \
          -lnvdsgst_meta -lnvds_meta -lnvds_yml_parser -lnvbufsurface \
          -lcuda -pthread

PROTO_CXXFLAGS= $(COMMON_CXXFLAGS) -O2 $(shell pkg-config --cflags protobuf)

TEST_PKGS:= glib-2.0 opencv4 protobuf
TEST_CXXFLAGS= $(COMMON_CXXFLAGS) -O1 \
               $(shell pkg-config --cflags $(TEST_PKGS)) \
               -DTEST_SRC_DIR='"$(CURDIR)"'
TEST_LIBS= $(shell pkg-config --libs $(TEST_PKGS)) -pthread

########################################################################
# Исходники
########################################################################

APP_SRCS:= $(wildcard *.cpp)
APP_OBJS:= $(APP_SRCS:%.cpp=$(BUILD_DIR)/%.o)

PROTOS:= $(wildcard proto/*.proto)
PROTO_SRCS:= $(PROTOS:proto/%.proto=$(GEN_DIR)/%.pb.cc)
PROTO_HDRS:= $(PROTO_SRCS:.cc=.h)
PROTO_OBJS:= $(PROTO_SRCS:.cc=.o)

TEST_APP_SRCS:= app_config.cpp geo_zone.cpp log.cpp pixel_transformer.cpp \
                points_sender.cpp udp_connection.cpp
TEST_SRCS:= $(wildcard tests/*.cpp)
TEST_OBJS:= $(TEST_APP_SRCS:%.cpp=$(TEST_DIR)/%.o) \
            $(TEST_SRCS:tests/%.cpp=$(TEST_DIR)/%.o)

########################################################################
# Окружение сборки
########################################################################

# В каждом каталоге сборки лежит файл с версиями компилятора, protoc и
# флагами. Он перезаписывается, только когда что-то из этого поменялось, и
# тогда всё в каталоге пересобирается. Так build/ не смешивает файлы,
# собранные на хосте и в контейнере (например, points.pb.* от protoc хоста
# не компилируются с libprotobuf контейнера).
TOOLCHAIN= $(shell $(CXX) -dumpfullversion) $(shell protoc --version)

# $(call write-if-changed,файл,текст): время файла меняется, только если
# текст другой, поэтому без изменений ничего не пересобирается
write-if-changed= printf '%s\n' '$(subst ','\'',$(2))' > $(1).new; \
                  cmp -s $(1).new $(1) && rm $(1).new || mv $(1).new $(1)

APP_ENV:= $(BUILD_DIR)/build-env
PROTO_ENV:= $(GEN_DIR)/build-env
TEST_ENV:= $(TEST_DIR)/build-env

$(APP_ENV): FORCE | $(BUILD_DIR)
	@$(call write-if-changed,$@,$(TOOLCHAIN) $(APP_CXXFLAGS) $(APP_LIBS))

$(PROTO_ENV): FORCE | $(GEN_DIR)
	@$(call write-if-changed,$@,$(TOOLCHAIN) $(PROTO_CXXFLAGS))

$(TEST_ENV): FORCE | $(TEST_DIR)
	@$(call write-if-changed,$@,$(TOOLCHAIN) $(TEST_CXXFLAGS) $(TEST_LIBS))

check-deepstream:
	@test -d $(DS_ROOT)/sources/includes || { \
	    echo "DeepStream не найден в $(DS_ROOT)." >&2; \
	    exit 1; }

$(BUILD_DIR) $(GEN_DIR) $(TEST_DIR) $(GRAPHS_DIR):
	mkdir -p $@

########################################################################
# Приложение
########################################################################

all: $(TARGET)

$(BUILD_DIR)/%.o: %.cpp $(APP_ENV) | check-deepstream $(PROTO_HDRS)
	$(CXX) -c -o $@ $(APP_CXXFLAGS) $<

$(TARGET): $(APP_OBJS) $(PROTO_OBJS) $(APP_ENV)
	$(CXX) -o $@ $(APP_OBJS) $(PROTO_OBJS) $(APP_LIBS)

run: $(TARGET)
	$(TARGET) $(RUN_CONFIG)

########################################################################
# Protobuf (общий для приложения и тестов)
########################################################################

$(PROTO_SRCS) $(PROTO_HDRS) &: $(PROTOS) $(PROTO_ENV)
	protoc -Iproto --cpp_out=$(GEN_DIR) $(PROTOS)

$(GEN_DIR)/%.pb.o: $(GEN_DIR)/%.pb.cc $(PROTO_ENV)
	$(CXX) -c -o $@ $(PROTO_CXXFLAGS) $<

########################################################################
# Юнит-тесты
#   make test                    собрать и запустить все тесты
#   make test TEST_FILTER=csv    только тесты, в имени которых есть подстрока
########################################################################

test: $(TEST_BIN)
	$(TEST_BIN) $(TEST_FILTER)

$(TEST_DIR)/%.o: %.cpp $(TEST_ENV) | $(PROTO_HDRS)
	$(CXX) -c -o $@ $(TEST_CXXFLAGS) $<

$(TEST_DIR)/%.o: tests/%.cpp $(TEST_ENV) | $(PROTO_HDRS)
	$(CXX) -c -o $@ $(TEST_CXXFLAGS) $<

$(TEST_BIN): $(TEST_OBJS) $(PROTO_OBJS) $(TEST_ENV)
	$(CXX) -o $@ $(TEST_OBJS) $(PROTO_OBJS) $(TEST_LIBS)

########################################################################
# TensorRT engine по model-engine-file из NVINFER_CONFIG. Строится, только
# если его нет или поменялись ONNX, GPU, TensorRT или параметры сборки
# (нужен GPU).
########################################################################

engine:
	bash tools/build_engine.sh $(NVINFER_CONFIG)

engine-rebuild:
	bash tools/build_engine.sh --force $(NVINFER_CONFIG)

########################################################################
# Граф конвейера для отладки (dot-файлы GStreamer, нужен graphviz)
########################################################################

gen-graph: $(TARGET) | $(GRAPHS_DIR)
	GST_DEBUG_DUMP_DOT_DIR=$(GRAPHS_DIR) $(TARGET) $(RUN_CONFIG)

graph:
	dot -Tpng $(GRAPHS_DIR)/sheremetevo.dot -o $(GRAPHS_DIR)/sheremetevo.png

########################################################################

clean:
	rm -rf $(BUILD_DIR) $(GRAPHS_DIR)

help:
	@echo "make                        собрать приложение ($(TARGET))"
	@echo "make run [RUN_CONFIG=...]   собрать и запустить (по умолчанию $(RUN_CONFIG))"
	@echo "make test [TEST_FILTER=...] собрать и запустить юнит-тесты"
	@echo "make engine                 построить TensorRT engine, если нужно"
	@echo "make engine-rebuild         построить engine заново"
	@echo "make gen-graph              запустить приложение с выгрузкой графа в $(GRAPHS_DIR)/"
	@echo "make graph                  нарисовать $(GRAPHS_DIR)/sheremetevo.png"
	@echo "make clean                  удалить $(BUILD_DIR)/ и $(GRAPHS_DIR)/"

.PHONY: all run test engine engine-rebuild gen-graph graph clean help \
        check-deepstream FORCE

-include $(wildcard $(BUILD_DIR)/*.d $(GEN_DIR)/*.d $(TEST_DIR)/*.d)
