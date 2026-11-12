// Cordgeist test-only bridge to the pinned upstream SOFTWARE FAKE libusb.
#include "libusb.h"
extern "C" int cg_test_discovery_seed(int mode) {
    aoahid_fake_libusb_reset();
    const uint8_t ports[] = {2, 3};
    aoahid_fake_libusb_device_config config{};
    config.bus = 1;
    config.address = 7;
    config.port_path = ports;
    config.port_path_length = 2;
    config.vendor_id = 1;
    config.product_id = 2;
    config.protocol_version = 2;
    config.serial = mode == 1 ? "\xff" : "SYNTHETIC_NATIVE_SERIAL";
    config.product = "SYNTHETIC_NATIVE_PRODUCT";
    config.open_status = mode == 2 ? LIBUSB_ERROR_ACCESS : LIBUSB_SUCCESS;
    return aoahid_fake_libusb_add_device(&config);
}
extern "C" int cg_test_discovery_verify(int mode) {
    const size_t expected = mode == 2 ? 0U : 1U;
    if (aoahid_fake_libusb_control_count() != expected ||
        aoahid_fake_libusb_claim_count() != 0U ||
        aoahid_fake_libusb_set_configuration_count() != 0U ||
        aoahid_fake_libusb_open_handle_count() != 0U ||
        aoahid_fake_libusb_pending_transfer_count() != 0U ||
        aoahid_fake_libusb_bulk_out_count() != 0U) return 0;
    for (size_t i = 0; i < expected; ++i) {
        aoahid_fake_libusb_control_record record{};
        if (aoahid_fake_libusb_get_control(i, &record) != LIBUSB_SUCCESS || record.request != 51U ||
            record.timeout == 0U || record.timeout > 500U || record.asynchronous) return 0;
    }
    return 1;
}

// Session lifecycle controls. All traffic stays in the marked SOFTWARE FAKE.
extern "C" void cg_test_session_close_error() {
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
}
extern "C" void cg_test_session_delay_callback() {
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, -1, 0xffffffffU);
}
extern "C" void cg_test_session_release_callback() {
    aoahid_fake_libusb_make_pending_transfers_ready();
}
extern "C" int cg_test_session_verify(int mode) {
    if (aoahid_fake_libusb_claim_count() != 0U ||
        aoahid_fake_libusb_set_configuration_count() != 0U ||
        aoahid_fake_libusb_open_handle_count() != 0U ||
        aoahid_fake_libusb_pending_transfer_count() != 0U ||
        aoahid_fake_libusb_bulk_out_count() != 0U) return 0;
    size_t registrations=0U, unregistrations=0U, reports=0U;
    uint16_t ids[2]{};
    uint16_t closed[2]{};
    for (size_t i=0U; i<aoahid_fake_libusb_control_count(); ++i) {
        aoahid_fake_libusb_control_record r{};
        if (aoahid_fake_libusb_get_control(i,&r)!=LIBUSB_SUCCESS || r.timeout==0U || r.timeout>1000U)
            return 0;
        if (r.request==54U) {
            if (registrations>=2U) return 0;
            ids[registrations++]=r.value;
        } else if (r.request==55U) {
            if (unregistrations>=2U) return 0;
            closed[unregistrations++]=r.value;
        } else if (r.request==57U) {
            if (mode!=1 || !r.asynchronous) return 0;
            unsigned char data[64]{};
            const size_t length=aoahid_fake_libusb_copy_control_data(i,data,sizeof(data));
            if (length==0U || length>sizeof(data)) return 0;
            bool all_zero=true;
            for (size_t j=0U;j<length;++j) all_zero=all_zero && data[j]==0U;
            if ((reports==0U && all_zero) || (reports==1U && !all_zero)) return 0;
            ++reports;
        } else if (r.request!=51U && r.request!=56U) return 0;
    }
    if (mode==2) return registrations==0U && unregistrations==0U && reports==0U;
    return registrations==2U && unregistrations==2U && closed[0]==ids[1] &&
        closed[1]==ids[0] && reports==(mode==1 ? 2U : 0U);
}

extern "C" void cg_test_session_submit_fault(int mode) {
    if (mode==1) aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL,0,0U);
    if (mode==2) aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,0,0U);
    if (mode==3) aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_TIMED_OUT,0,0U);
}
extern "C" int cg_test_session_reports_verify() {
    if (aoahid_fake_libusb_open_handle_count()!=0U || aoahid_fake_libusb_pending_transfer_count()!=0U ||
        aoahid_fake_libusb_claim_count()!=0U || aoahid_fake_libusb_set_configuration_count()!=0U) return 0;
    size_t reports=0U;
    for(size_t i=0U;i<aoahid_fake_libusb_control_count();++i) {
        aoahid_fake_libusb_control_record r{};
        if(aoahid_fake_libusb_get_control(i,&r)!=LIBUSB_SUCCESS) return 0;
        if(r.request!=57U) continue;
        if(r.timeout==0U || r.timeout>1000U || reports>=2U) return 0;
        unsigned char data[64]{};
        size_t n=aoahid_fake_libusb_copy_control_data(i,data,sizeof(data));
        if(n==0U || n>sizeof(data)) return 0;
        bool neutral=true;
        for(size_t j=0U;j<n;++j) neutral=neutral && data[j]==0U;
        if(reports>0U && !neutral) return 0; // cleanup only, never retry pressed state
        ++reports;
    }
    return reports>=1U;
}
extern "C" int cg_test_session_neutral_only_verify() {
    if(aoahid_fake_libusb_open_handle_count()!=0U || aoahid_fake_libusb_pending_transfer_count()!=0U) return 0;
    for(size_t i=0U;i<aoahid_fake_libusb_control_count();++i) {
        aoahid_fake_libusb_control_record r{};
        if(aoahid_fake_libusb_get_control(i,&r)!=LIBUSB_SUCCESS) return 0;
        if(r.request!=57U) continue;
        unsigned char data[64]{};size_t n=aoahid_fake_libusb_copy_control_data(i,data,sizeof(data));
        if(n==0U || n>sizeof(data)) return 0;
        for(size_t j=0U;j<n;++j) if(data[j]!=0U) return 0;
    }
    return 1;
}
extern "C" size_t cg_test_session_report_count() {
    size_t count=0U;
    for(size_t i=0U;i<aoahid_fake_libusb_control_count();++i) {
        aoahid_fake_libusb_control_record r{};
        if(aoahid_fake_libusb_get_control(i,&r)!=LIBUSB_SUCCESS) return static_cast<size_t>(-1);
        if(r.request==57U) ++count;
    }
    return count;
}
extern "C" int cg_test_session_clean_verify() {
    return aoahid_fake_libusb_open_handle_count()==0U &&
        aoahid_fake_libusb_pending_transfer_count()==0U &&
        aoahid_fake_libusb_claim_count()==0U &&
        aoahid_fake_libusb_set_configuration_count()==0U;
}
extern "C" void cg_test_gesture_release_fault() {
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,-1,0U);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL,0,0U);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL,0,0U);
}
// Synthetic expected wire states for US ANSI A, !, Tab, LF. No text is logged.
extern "C" int cg_test_ascii_policy_verify() {
    const unsigned char modifiers[12]={2,2,2,0,2,2,2,0,0,0,0,0};
    const unsigned short usages[12]={0,4,0,0,0,0x1e,0,0,0x2b,0,0x28,0};
    size_t index=0U;
    for(size_t i=0U;i<aoahid_fake_libusb_control_count();++i) {
        aoahid_fake_libusb_control_record r{};
        if(aoahid_fake_libusb_get_control(i,&r)!=LIBUSB_SUCCESS) return 0;
        if(r.request!=57U) continue;
        if(index>=12U || r.timeout==0U || r.timeout>500U) return 0;
        unsigned char data[64]{};
        const size_t n=aoahid_fake_libusb_copy_control_data(i,data,sizeof(data));
        if(n!=29U) return 0;
        unsigned char expected[29]{};expected[0]=modifiers[index];
        if(usages[index]!=0U) {const size_t bit=usages[index]-4U;expected[1U+bit/8U]=static_cast<unsigned char>(1U<<(bit%8U));}
        for(size_t j=0U;j<n;++j) if(data[j]!=expected[j]) return 0;
        ++index;
    }
    return index==12U && aoahid_fake_libusb_open_handle_count()==0U && aoahid_fake_libusb_pending_transfer_count()==0U;
}
