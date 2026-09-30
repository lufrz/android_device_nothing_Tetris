#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

import hashlib

from extract_utils.file import File
from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/nothing/Tetris',
    'hardware/mediatek',
    'hardware/mediatek/libmtkperf_client',
]

def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None

def blob_fixup_goodix_ui_ready_timeout(ctx, file: File, file_path: str,
                                     *args, **kwargs):
    # Screen-off pulses can exceed the stock 450 ms UI-ready deadline. Extend
    # only the maximum wait to 1000 ms; readiness and finger-up still wake it.
    original_hash = 'e9766a94fa3cf501de3449f37c45676ea9fe545fc3da0bcead65e1ae63245c07'
    patched_hash = 'f80d3c632b76ad4d0b2e5de44dae604f75269deaff3d87b4de046336e993f92f'
    offset = 0x31620
    original_instruction = bytes.fromhex('41 38 80 52')  # mov w1, #450
    patched_instruction = bytes.fromhex('01 7d 80 52')  # mov w1, #1000

    with open(file_path, 'r+b') as blob:
        data = blob.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest == patched_hash:
            return
        if digest != original_hash:
            raise ValueError(f'Unsupported libgf_hal.so UI-ready fixup: {digest}')
        if data[offset:offset + 4] != original_instruction:
            raise ValueError('Unexpected Goodix UI-ready wait instruction')

        patched = data[:offset] + patched_instruction + data[offset + 4:]
        if hashlib.sha256(patched).hexdigest() != patched_hash:
            raise ValueError('Unexpected Goodix UI-ready fixup result')
        blob.seek(offset)
        blob.write(patched_instruction)


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'libneuron_graph_delegate.mtk',
        'libtflite_mtk',
        'vendor.mediatek.hardware.apuware.utils@2.0',
        'vendor.mediatek.hardware.videotelephony@1.0'
    ): lib_fixup_vendor_suffix,
}

blob_fixups: blob_fixups_user_type = {
    'vendor/lib64/libgf_hal.so': blob_fixup()
        .call(blob_fixup_goodix_ui_ready_timeout, need_tmp_dir=False),
    (
        'system_ext/etc/init/init.vtservice.rc',
        'vendor/etc/init/android.hardware.neuralnetworks-shim-service-mtk.rc'
    ): blob_fixup()
        .regex_replace('start', 'enable'),
    'system_ext/lib64/libimsma.so': blob_fixup()
        .replace_needed('libsink.so', 'libsink-mtk.so'),
    'system_ext/lib64/libsink-mtk.so': blob_fixup()
        .add_needed('libaudioclient_shim.so'),
    'system_ext/priv-app/ImsService/ImsService.apk': blob_fixup()
        .apktool_patch('blob-patches'),
    'vendor/bin/hw/android.hardware.media.c2@1.2-mediatek-64b': blob_fixup()
        .replace_needed('libcodec2_hidl@1.0.so', 'libcodec2_hidl@1.0-v33.so')
        .replace_needed('libcodec2_hidl@1.1.so', 'libcodec2_hidl@1.1-v33.so')
        .replace_needed('libcodec2_hidl@1.2.so', 'libcodec2_hidl@1.2-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so'),
    'vendor/etc/wifi/p2p_supplicant_overlay.conf': blob_fixup()
        .add_line_if_missing('p2p_go_vht=1'),
    'vendor/etc/wifi/wpa_supplicant.conf': blob_fixup()
        .add_line_if_missing('rsn_overriding=1'),
    'vendor/lib64/hw/android.hardware.audio@7.1-impl-mediatek.so': blob_fixup()
        .replace_needed('android.hardware.audio@7.1-util.so', 'android.hardware.audio@7.1-util-v34.so'),
    'vendor/lib64/hw/audio.primary.mediatek.so': blob_fixup()
        .add_needed('libstagefright_foundation-v33.so')
        .replace_needed('libalsautils.so', 'libalsautils-v33.so')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so')
        .binary_regex_replace(b'A2dpsuspendonly', b'A2dpSuspended\x00\x00')
        .binary_regex_replace(b'BTAudiosuspend', b'A2dpSuspended\x00'),
    'vendor/lib64/hw/hwcomposer.mtk_common.so': blob_fixup()
        .add_needed('libprocessgroup_shim.so'),
    'vendor/bin/mnld': blob_fixup()
        .replace_needed('libmnl.so', 'libmnl_mtk.so'),
    'vendor/lib64/hw/sensors.mediatek.V2.0.so': blob_fixup()
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    (
        'vendor/lib64/mt6878/libcameracustom.imgsensor.core.so',
        'vendor/lib64/mt6878/libcameracustom.so',
    ): blob_fixup()
        .add_needed('libmtk_cam_shim_Tetris.so')
        .remove_needed('s5kgn9sp_mipi_raw_IdxMgr.so')
        .remove_needed('s5kgn9spofxian_mipi_raw_IdxMgr.so')
        .remove_needed('gc08a8_mipi_raw_IdxMgr.so')
        .remove_needed('gc08a8xl_mipi_raw_IdxMgr.so')
        .remove_needed('gc08a8syx_mipi_raw_IdxMgr.so')
        .remove_needed('ov50d40_mipi_raw_IdxMgr.so')
        .remove_needed('ov50d40ofilm_mipi_raw_IdxMgr.so')
        .remove_needed('gc02m1_mipi_raw_IdxMgr.so')
        .remove_needed('mtk000_mipi_raw_IdxMgr.so'),
    'vendor/lib64/mt6878/libmtkcam_thirdparty.customer.so': blob_fixup()
        .remove_needed('libarcsoft_watermark.so')
        .remove_needed('libarcsoft_portrait_super_night_raw.so')
        .remove_needed('libarcsoft_superportrait.so')
        .remove_needed('libarcsoft_super_night_raw.so')
        .remove_needed('libarcsoft_scbokeh_image.so')
        .remove_needed('libarcsoft_scbokeh_preview.so')
        .remove_needed('libarcsoft_mf_superresolution.so')
        .remove_needed('libmouth_mask_detection.arcsoft.so')
        .remove_needed('libarcsoft_portrait_distortion_correction.so')
        .remove_needed('libarcsoft_dark_vision_raw.so')
        .remove_needed('libarcsoft_dualcam_refocus_video.so')
        .remove_needed('libarcsoft_dualcam_refocus_image.so')
        .remove_needed('libarcsoft_beautyshot.so')
        .remove_needed('libarcsoft_aiscenedetection.so')
        .remove_needed('libarcsoft_high_dynamic_range_v5.so'),
    'vendor/bin/hw/mt6878/camerahalserver': blob_fixup()
        .add_needed('libcamera_metadata_shim.so'),
    (
        'vendor/lib64/mt6878/libcam.hal3a.so',
        'vendor/lib64/mt6878/libcam.hal3a.ctrl.so',
        'vendor/lib64/mt6878/libmtkcam_request_requlator.so',
        'vendor/lib64/libmtkcam_cputrack.so',
    ): blob_fixup()
        .add_needed('libprocessgroup_shim.so'),
    'vendor/lib64/libntcamcore.so': blob_fixup()
        .remove_needed('libntcamextened.so'),
    (
        'vendor/bin/hw/mt6878/android.hardware.graphics.allocator-V2-service-mediatek.mt6878',
        'vendor/lib64/egl/mt6878/libGLES_mali.so',
        'vendor/lib64/hw/mt6878/android.hardware.graphics.allocator-V2-mediatek.so',
        'vendor/lib64/hw/mt6878/android.hardware.graphics.mapper@4.0-impl-mediatek.so',
        'vendor/lib64/hw/mt6878/mapper.mediatek.so',
        'vendor/lib64/libaimemc.so',
        'vendor/lib64/libcodec2_fsr.so',
        'vendor/lib64/libcodec2_vpp_AIMEMC_plugin.so',
        'vendor/lib64/libcodec2_vpp_AISR_plugin.so',
        'vendor/lib64/libmtkcam_grallocutils.so',
        'vendor/lib64/mt6878/libmtkcam_grallocutils.so',
        'vendor/lib64/libmtkcam_grallocutils_aidlv1helper.so',
        'vendor/lib64/vendor.mediatek.hardware.camera.isphal-V1-ndk.so',
        'vendor/lib64/vendor.mediatek.hardware.pq_aidl-V2-ndk.so',
        'vendor/lib64/vendor.mediatek.hardware.pq_aidl-V4-ndk.so',
        'vendor/lib64/vendor.mediatek.hardware.pq_aidl-V7-ndk.so',
    ): blob_fixup()
        .replace_needed('android.hardware.graphics.common-V4-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    (
        'vendor/lib64/libmtkcam_grallocutils.so',
        'vendor/lib64/mt6878/libmtkcam_grallocutils.so',
        'vendor/lib64/libmtkcam_grallocutils_aidlv1helper.so',
        'vendor/lib64/libntcamcore.so',
    ): blob_fixup()
        .replace_needed('android.hardware.graphics.allocator-V1-ndk.so', 'android.hardware.graphics.allocator-V2-ndk.so'),
    (
        'vendor/lib64/mt6878/libdpframework.so',
        'vendor/lib64/libpqsharememory.so',
    ): blob_fixup()
        .replace_needed('vendor.mediatek.hardware.pq_aidl-V2-ndk.so', 'vendor.mediatek.hardware.pq_aidl-V7-ndk.so'),
    'vendor/lib64/hw/hwcomposer.mtk_common.so': blob_fixup()
        .replace_needed('vendor.mediatek.hardware.pq_aidl-V4-ndk.so', 'vendor.mediatek.hardware.pq_aidl-V7-ndk.so'),
    'vendor/lib64/mt6878/libpqconfig.so': blob_fixup()
        .replace_needed('android.hardware.sensors-V2-ndk.so', 'android.hardware.sensors-V3-ndk.so'),
    'vendor/lib64/vendor.mediatek.hardware.bluetooth.audio-V1-ndk.so': blob_fixup()
        .replace_needed('android.hardware.audio.common-V1-ndk.so', 'android.hardware.audio.common-V2-ndk.so'),
    'vendor/lib64/mt6878/libmtkcam_hal_aidl_common.so': blob_fixup()
        .replace_needed('android.hardware.camera.common-V2-ndk.so', 'android.hardware.camera.common-V1-ndk.so'),
    'vendor/lib64/mt6878/libmorpho_video_stabilizer.so': blob_fixup()
        .add_needed('libutils.so'),
    'vendor/lib64/mt6878/libneuralnetworks_sl_driver_mtk_prebuilt.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_createFromHandle')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock')
        .add_needed('libbase_shim.so'),
    'vendor/lib64/libmorpho_RapidEffect.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    'vendor/lib64/libcodec2_hidl@1.0-v33.so': blob_fixup()
        .replace_needed('libstagefright_bufferqueue_helper.so', 'libstagefright_bufferqueue_helper-bp2a.so')
        .replace_needed('libcodec2_hidl_plugin.so', 'libcodec2_hidl_plugin-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/libcodec2_hidl@1.1-v33.so': blob_fixup()
        .replace_needed('libstagefright_bufferqueue_helper.so', 'libstagefright_bufferqueue_helper-bp2a.so')
        .replace_needed('libcodec2_hidl@1.0.so', 'libcodec2_hidl@1.0-v33.so')
        .replace_needed('libcodec2_hidl_plugin.so', 'libcodec2_hidl_plugin-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/libcodec2_hidl@1.2-v33.so': blob_fixup()
        .replace_needed('libstagefright_bufferqueue_helper.so', 'libstagefright_bufferqueue_helper-bp2a.so')
        .replace_needed('libcodec2_hidl@1.0.so', 'libcodec2_hidl@1.0-v33.so')
        .replace_needed('libcodec2_hidl@1.1.so', 'libcodec2_hidl@1.1-v33.so')
        .replace_needed('libcodec2_hidl_plugin.so', 'libcodec2_hidl_plugin-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/libcodec2_hidl_plugin-v33.so': blob_fixup()
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so'),
    (
        'vendor/lib64/libcodec2_mtk_c2store.so',
        'vendor/lib64/libcodec2_vpp_fa_plugin.so',
        'vendor/lib64/libcodec2_vpp_mi_plugin.so',
        'vendor/lib64/libcodec2_vpp_qt_plugin.so',
        'vendor/lib64/libcodec2_vpp_rs_plugin.so',
    ): blob_fixup()
        .replace_needed('libcodec2_soft_common.so', 'libcodec2_soft_common-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so')
        .replace_needed('libsfplugin_ccodec_utils.so', 'libsfplugin_ccodec_utils-v33.so'),
    (
        'vendor/lib64/libcodec2_mtk_vdec.so',
        'vendor/lib64/libcodec2_mtk_venc.so',
    ): blob_fixup()
        .replace_needed('libcodec2_soft_common.so', 'libcodec2_soft_common-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libformatter.so', 'libformatter_mtk.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so')
        .replace_needed('libsfplugin_ccodec_utils.so', 'libsfplugin_ccodec_utils-v33.so')
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/libcodec2_soft_common-v33.so': blob_fixup()
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so')
        .replace_needed('libsfplugin_ccodec_utils.so', 'libsfplugin_ccodec_utils-v33.so'),
    'vendor/lib64/libcodec2_vndk-v33.so': blob_fixup()
        .remove_needed('android.hardware.media.bufferpool2-V1-ndk.so')
        .replace_needed('libui.so', 'libui-v34.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    (
        'vendor/lib64/libcodec2_vpp_AIMEMC_plugin.so',
        'vendor/lib64/libcodec2_vpp_AISR_plugin.so',
    ): blob_fixup()
        .replace_needed('libcodec2_soft_common.so', 'libcodec2_soft_common-v33.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so')
        .replace_needed('libsfplugin_ccodec_utils.so', 'libsfplugin_ccodec_utils-v33.so')
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/libsfplugin_ccodec_utils-v33.so': blob_fixup()
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v33.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    'vendor/lib64/mt6878/libneuron_adapter_mc.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_describe'),
    'vendor/lib64/libntcamskia.so': blob_fixup()
        .add_needed('libnativewindow.so'),
    'vendor/bin/hw/mtkfusionrild': blob_fixup()
        .add_needed('libutils-v33.so'),
    'vendor/lib64/libnvram.so': blob_fixup()
        .add_needed('libbase_shim.so'),

    'vendor/lib64/hw/mt6878/vendor.mediatek.hardware.pq_aidl-impl.so': blob_fixup()
        .add_needed('libui_shim.so')
        .replace_needed('libui.so', 'libui-v34.so')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),
    (
        'vendor/lib64/mt6878/libmmlpqImpl.so',
        'vendor/lib64/libpqxmlflagparser.so',
        'vendor/lib64/libpqxmlparser.so',
        'vendor/lib64/libsilkybrightnesscore.so',
        'vendor/lib64/librt_extamp_intf.so',
    ): blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),

    'vendor/lib64/mt6878/lib3a.ae.stat.so': blob_fixup()
        .add_needed('liblog.so'),
    'vendor/lib64/libarmnn_ndk.mtk.vndk.so': blob_fixup()
        .add_needed('liblog.so'),
}  # fmt: skip

module = ExtractUtilsModule(
    'Tetris',
    'nothing',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=True,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
