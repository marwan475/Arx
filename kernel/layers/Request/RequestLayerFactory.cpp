#include "layers/Request/RequestLayerFactory.hpp"

RequestLayerFactory::RequestLayerFactory()
{
    RequestLayerExportCaps = nullptr;
}

RequestLayerFactory::~RequestLayerFactory()
{
    if (RequestLayerExportCaps != nullptr)
    {
        delete RequestLayerExportCaps->processRequestManager;
        delete RequestLayerExportCaps->schedulerRequestManager;
        delete RequestLayerExportCaps->timeRequestManager;
        delete RequestLayerExportCaps->memoryRequestManager;
        delete RequestLayerExportCaps->vfsRequestManager;
        delete RequestLayerExportCaps->eventRequestManager;
        delete RequestLayerExportCaps->syncRequestManager;
        delete RequestLayerExportCaps->signalRequestManager;
        delete RequestLayerExportCaps->credentialRequestManager;
        delete RequestLayerExportCaps->systemRequestManager;
        delete RequestLayerExportCaps->socketRequestManager;
        delete RequestLayerExportCaps->interruptRequestManager;
        delete RequestLayerExportCaps;
        RequestLayerExportCaps = nullptr;
    }
}

RequestLayerCaps* RequestLayerFactory::Create()
{
    if (RequestLayerExportCaps != nullptr)
    {
        return RequestLayerExportCaps;
    }

    RequestLayerExportCaps                           = new RequestLayerCaps();
    RequestLayerExportCaps->processRequestManager    = new ProcessRequestManager();
    RequestLayerExportCaps->schedulerRequestManager  = new SchedulerRequestManager();
    RequestLayerExportCaps->timeRequestManager       = new TimeRequestManager();
    RequestLayerExportCaps->memoryRequestManager     = new MemoryRequestManager();
    RequestLayerExportCaps->vfsRequestManager        = new VfsRequestManager();
    RequestLayerExportCaps->eventRequestManager      = new EventRequestManager();
    RequestLayerExportCaps->syncRequestManager       = new SyncRequestManager();
    RequestLayerExportCaps->signalRequestManager     = new SignalRequestManager();
    RequestLayerExportCaps->credentialRequestManager = new CredentialRequestManager();
    RequestLayerExportCaps->systemRequestManager     = new SystemRequestManager();
    RequestLayerExportCaps->socketRequestManager     = new SocketRequestManager();
    RequestLayerExportCaps->interruptRequestManager  = new InterruptRequestManager();

    return RequestLayerExportCaps;
}

RequestLayerCaps* RequestLayerFactory::GetCaps() const
{
    return RequestLayerExportCaps;
}
