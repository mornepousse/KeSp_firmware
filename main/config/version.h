#ifndef VERSION_H
#define VERSION_H

// Version is defined in top-level CMakeLists.txt via add_compile_definitions()
// Do NOT redefine VERSION_1 or VERSION_2 here

#ifdef VERSION_1
    #define GATTS_TAG "KaSe_V1"
    #define MANUFACTURER_NAME "Mae"
    #define PRODUCT_NAME "KaSe V1"
    #define SERIAL_NUMBER "N/A"
    #define MODULE_ID 0x01
#elif defined(VERSION_2)
    #define GATTS_TAG "KaSe_V2"
    #define MANUFACTURER_NAME "Mae"
    #define PRODUCT_NAME "KaSe V2"
    #define SERIAL_NUMBER "N/A"
    #define MODULE_ID 0x02
#elif defined(VERSION_2_DEBUG)
    #define GATTS_TAG "KaSe_V2_DBG"
    #define MANUFACTURER_NAME "Mae"
    #define PRODUCT_NAME "KaSe V2 Debug"
    #define SERIAL_NUMBER "N/A"
    #define MODULE_ID 0x02
#else
    #error "No hardware version defined. Set VERSION_1, VERSION_2, or VERSION_2_DEBUG in CMakeLists.txt"
#endif

#endif // VERSION_H
