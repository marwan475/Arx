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

RequestLayerCaps* RequestLayerFactory::Create(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps)
{
    if (RequestLayerExportCaps != nullptr)
    {
        return RequestLayerExportCaps;
    }

    RequestLayerExportCaps                           = new RequestLayerCaps();
    RequestLayerExportCaps->processRequestManager    = new ProcessRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->schedulerRequestManager  = new SchedulerRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->timeRequestManager       = new TimeRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->memoryRequestManager     = new MemoryRequestManager(logicLayerCaps);
    RequestLayerExportCaps->vfsRequestManager        = new VfsRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->eventRequestManager      = new EventRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->syncRequestManager       = new SyncRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->signalRequestManager     = new SignalRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->credentialRequestManager = new CredentialRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->systemRequestManager     = new SystemRequestManager(resourceLayerCaps);
    RequestLayerExportCaps->socketRequestManager     = new SocketRequestManager(resourceLayerCaps, logicLayerCaps);
    RequestLayerExportCaps->interruptRequestManager  = new InterruptRequestManager();

    return RequestLayerExportCaps;
}

RequestLayerCaps* RequestLayerFactory::GetCaps() const
{
    return RequestLayerExportCaps;
}
