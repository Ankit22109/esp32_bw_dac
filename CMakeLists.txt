cmake_minimum_required(VERSION 3.22)

# Tell CMake exactly where the internal Bluetooth utilities live inside the v6.0.1 framework container
set(EXTRA_COMPONENT_DIRS 
    "$ENV{IDF_PATH}/examples/bluetooth/bluedroid/classic_bt/common/bt_app_core_utils"
    "$ENV{IDF_PATH}/examples/bluetooth/bluedroid/classic_bt/common/bredr_app_common_utils"
    "$ENV{IDF_PATH}/examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_common_utils"
    "$ENV{IDF_PATH}/examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_int_codec_utils"
    "$ENV{IDF_PATH}/examples/bluetooth/bluedroid/classic_bt/common/a2dp_utils/a2dp_sink_ext_codec_utils"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(bluetooth_DAC)