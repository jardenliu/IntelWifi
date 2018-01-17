/* add your code here */

#include "IntelWifi.hpp"

extern "C" {
#include "Configuration.h"
}

#include <IOKit/IOInterruptController.h>


#include <sys/errno.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(IntelWifi, IOEthernetController)



enum {
    kOffPowerState,
    kOnPowerState,
    kNumPowerStates
};

static IOPMPowerState gPowerStates[kNumPowerStates] = {
    // kOffPowerState
    {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    // kOnPowerState
    {kIOPMPowerStateVersion1, (kIOPMPowerOn | kIOPMDeviceUsable), kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};


static struct MediumTable
{
    IOMediumType type;
    UInt32 speed;
} mediumTable[] = {
    {kIOMediumIEEE80211None, 0},
    {kIOMediumIEEE80211Auto, 0}
};


int IntelWifi::findMSIInterruptTypeIndex()
{
    IOReturn ret;
    int index, source = 0;
    for (index = 0; ; index++)
    {
        int interruptType;
        ret = pciDevice->getInterruptType(index, &interruptType);
        if (ret != kIOReturnSuccess)
            break;
        if (interruptType & kIOInterruptTypePCIMessaged)
        {
            source = index;
            break;
        }
    }
    return source;
}

bool IntelWifi::init(OSDictionary *properties) {
    TraceLog("Driver init()");
    
    return super::init(properties);
}

void IntelWifi::free() {
    TraceLog("Driver free()");
    
    releaseAll();
    
    TraceLog("Fully finished");
    super::free();
}

bool IntelWifi::start(IOService *provider) {
    TraceLog("Driver start");
    
    if (!super::start(provider)) {
        TraceLog("Super start call failed!");
        return false;
    }
    
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        TraceLog("Provider is not PCI device");
        return false;
    }
    pciDevice->retain();
    
    fDeviceId = pciDevice->configRead16(kIOPCIConfigDeviceID);
    fSubsystemId = pciDevice->configRead16(kIOPCIConfigSubSystemID);
    
    fConfiguration = getConfiguration(fDeviceId, fSubsystemId);
    
    if (!fConfiguration) {
        TraceLog("ERROR: Failed to match configuration!");
        releaseAll();
        return false;
    }
    
    fTrans = iwl_trans_pcie_alloc(fConfiguration);
    if (!fTrans) {
        TraceLog("iwl_trans_pcie_alloc failed");
        releaseAll();
        return false;
    }
    
#ifdef CONFIG_IWLMVM
    const struct iwl_cfg *cfg_7265d = NULL;

    /*
     * special-case 7265D, it has the same PCI IDs.
     *
     * Note that because we already pass the cfg to the transport above,
     * all the parameters that the transport uses must, until that is
     * changed, be identical to the ones in the 7265D configuration.
     */
    if (fConfiguration == &iwl7265_2ac_cfg)
        cfg_7265d = &iwl7265d_2ac_cfg;
    else if (fConfiguration == &iwl7265_2n_cfg)
        cfg_7265d = &iwl7265d_2n_cfg;
    else if (fConfiguration == &iwl7265_n_cfg)
        cfg_7265d = &iwl7265d_n_cfg;
    if (cfg_7265d &&
        (fTrans->hw_rev & CSR_HW_REV_TYPE_MSK) == CSR_HW_REV_TYPE_7265D) {
        fConfiguration = cfg_7265d;
        fTrans->cfg = cfg_7265d;
    }
    
    if (fTrans->cfg->rf_id && fConfiguration == &iwla000_2ac_cfg_hr_cdb &&
        fTrans->hw_rev != CSR_HW_REV_TYPE_HR_CDB) {
        u32 rf_id_chp = CSR_HW_RF_ID_TYPE_CHIP_ID(fTrans->hw_rf_id);
        u32 jf_chp_id = CSR_HW_RF_ID_TYPE_CHIP_ID(CSR_HW_RF_ID_TYPE_JF);
        u32 hr_chp_id = CSR_HW_RF_ID_TYPE_CHIP_ID(CSR_HW_RF_ID_TYPE_HR);
        
        if (rf_id_chp == jf_chp_id) {
            if (fTrans->hw_rev == CSR_HW_REV_TYPE_QNJ)
                fConfiguration = &iwla000_2ax_cfg_qnj_jf_b0;
            else
                fConfiguration = &iwla000_2ac_cfg_jf;
        } else if (rf_id_chp == hr_chp_id) {
            if (fTrans->hw_rev == CSR_HW_REV_TYPE_QNJ)
                fConfiguration = &iwla000_2ax_cfg_qnj_hr_a0;
            else
                fConfiguration = &iwla000_2ac_cfg_hr;
        }
        fTrans->cfg = fConfiguration;
    }
#endif

    
    fTrans->drv = iwl_drv_start(fTrans);
    if (!fTrans->drv) {
        TraceLog("DRV init failed!");
        releaseAll();
        return false;
    }
    
    /* if RTPM is in use, enable it in our device */
    if (fTrans->runtime_pm_mode != IWL_PLAT_PM_MODE_DISABLED) {
        /* We explicitly set the device to active here to
         * clear contingent errors.
         */
        PMinit();
        provider->joinPMtree(this);
        setIdleTimerPeriod(iwlwifi_mod_params.d0i3_timeout);
        changePowerStateTo(kOffPowerState);// Set the public power state to the lowest level
        registerPowerDriver(this, gPowerStates, kNumPowerStates);
        makeUsable();
    }
    
    eeprom = IntelEeprom::withIO(io, const_cast<struct iwl_cfg*>(fConfiguration), fTrans->hw_rev);
    if (!eeprom) {
        TraceLog("EEPROM init failed!");
        releaseAll();
        return false;
    }
    
    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        TraceLog("getWorkLoop failed!");
        releaseAll();
        return false;
    }
    
    fWorkLoop->retain();
    
    int source = findMSIInterruptTypeIndex(); // Currently not using MSI because I want to see a lot of ignored interrupts in console
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
                                                                    (IOInterruptEventAction) &IntelWifi::interruptOccured,
                                                                    provider, source);
    if (!fInterruptSource) {
        TraceLog("InterruptSource init failed!");
        releaseAll();
        return false;
    }
    
    if (fWorkLoop->addEventSource(fInterruptSource) != kIOReturnSuccess) {
        TraceLog("EventSource registration failed");
        releaseAll();
        return false;
    }
    
    fInterruptSource->enable();
    
    
    opmode = new IwlDvmOpMode(this, io, eeprom);
    opmode = (IwlDvmOpMode *)opmode->start(fTrans, fTrans->cfg, &fTrans->drv->fw, NULL);
    
    if (!opmode) {
        //TraceLog("ERROR: Error while preparing HW: %d", err);
        releaseAll();
        return false;
    }
    
    struct iwl_trans_pcie* trans_pcie = IWL_TRANS_GET_PCIE_TRANS(fTrans);
    
    /* Set is_down to false here so that...*/
    trans_pcie->is_down = false;
    

    
    
//    fNvmData = eeprom->parse();
//    if (!fNvmData) {
//        TraceLog("EEPROM parse failed!");
//        releaseAll();
//        return false;
//    }
    
//    DebugLog("MAC: " MAC_FMT "\n"
//             "Num addr: %d\n"
//             "Calib: version - %d, voltage - %d\n"
//             "Raw temperature: %u",
//             MAC_BYTES(fNvmData->hw_addr),
//             fNvmData->n_hw_addrs,
//             fNvmData->calib_version, fNvmData->calib_voltage,
//             fNvmData->raw_temperature);
    
    if (!createMediumDict()) {
        TraceLog("MediumDict creation failed!");
        releaseAll();
        return false;
    }
    
    
    
//    if (!attachInterface((IONetworkInterface**)&netif)) {
//        TraceLog("Interface attach failed!");
//        releaseAll();
//        return false;
//    }
    
    
//    const struct fw_img *fw = iwl_get_ucode_image(&fTrans->drv->fw, IWL_UCODE_INIT);
//
//    iwl_trans_pcie_start_fw(fTrans, fw, false);
    
//    netif->registerService();

    
    //registerService();
    
    
    
    return true;
}

void IntelWifi::stop(IOService *provider) {
    
    iwl_trans_pcie_stop_device(fTrans, true);
    
    iwl_drv_stop(fTrans->drv);
    iwl_trans_pcie_free(fTrans);
    fTrans = NULL;
    
    if (netif) {
        detachInterface(netif);
        netif = NULL;
    }
    
    if (fWorkLoop) {
        if (fInterruptSource) {
            fInterruptSource->disable();
            fWorkLoop->removeEventSource(fInterruptSource);
        }
    }
    
    PMstop();
    
    super::stop(provider);
    TraceLog("Stopped");
}

bool IntelWifi::createMediumDict() {
    UInt32 capacity = sizeof(mediumTable) / sizeof(struct MediumTable);
    
    mediumDict = OSDictionary::withCapacity(capacity);
    if (mediumDict == 0) {
        return false;
    }
    
    for (UInt32 i = 0; i < capacity; i++) {
        IONetworkMedium* medium = IONetworkMedium::medium(mediumTable[i].type, mediumTable[i].speed);
        if (medium) {
            IONetworkMedium::addMedium(mediumDict, medium);
            medium->release();
        }
    }
    
    if (!publishMediumDictionary(mediumDict)) {
        return false;
    }
    
    IONetworkMedium *m = IONetworkMedium::getMediumWithType(mediumDict, kIOMediumIEEE80211Auto);
    setSelectedMedium(m);
    return true;
}


IOReturn IntelWifi::enable(IONetworkInterface *netif) {
    TraceLog("enable");
    
    IOMediumType mediumType = kIOMediumIEEE80211Auto;
    IONetworkMedium *medium = IONetworkMedium::getMediumWithType(mediumDict, mediumType);
    setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, medium);
    return kIOReturnSuccess;
}



IOReturn IntelWifi::disable(IONetworkInterface *netif) {
    TraceLog("disable");
//    netif->flushInputQueue();
    return kIOReturnSuccess;
}



IOReturn IntelWifi::getHardwareAddress(IOEthernetAddress *addrP) {
//    memcpy(addrP->bytes, fNvmData->hw_addr, 6);
    return kIOReturnSuccess;
}



IOReturn IntelWifi::setHardwareAddress(const IOEthernetAddress *addrP) { 
    return kIOReturnSuccess;
}



UInt32 IntelWifi::outputPacket(mbuf_t m, void *param) { 
    return 0;
}


bool IntelWifi::configureInterface(IONetworkInterface *netif) {
    TraceLog("Configure interface");
    if (!super::configureInterface(netif)) {
        return false;
    }
    
    IONetworkData *nd = netif->getNetworkData(kIONetworkStatsKey);
    if (!nd || !(fNetworkStats = (IONetworkStats*)nd->getBuffer())) {
        return false;
    }
    
    nd = netif->getNetworkData(kIOEthernetStatsKey);
    if (!nd || !(fEthernetStats = (IOEthernetStats*)nd->getBuffer())) {
        return false;
    }
    
    return true;
}

const OSString* IntelWifi::newVendorString() const {
    return OSString::withCString("Intel");
}


const OSString* IntelWifi::newModelString() const {
    return OSString::withCString(fConfiguration->name);
}

bool IntelWifi::interruptFilter(OSObject* owner, IOFilterInterruptEventSource * src) {
    src->signalInterrupt();
    return false;
}

void IntelWifi::interruptOccured(OSObject* owner, IOInterruptEventSource* sender, int count) {
    DebugLog("Interrupt");
    IntelWifi* me = (IntelWifi*)owner;
    
    if (me == 0) {
        return;
    }
    
    me->iwl_pcie_irq_handler(0, me->fTrans);
    
}
