#pragma once

struct ResourceLayerCaps;

struct LogicLayerCaps;
void test_memory_integration(ResourceLayerCaps*, LogicLayerCaps*);
void test_event_integration(ResourceLayerCaps*, LogicLayerCaps*);
void test_signal_integration(ResourceLayerCaps*, LogicLayerCaps*);
void test_communication_integration(ResourceLayerCaps*, LogicLayerCaps*);
void test_wait_integration(ResourceLayerCaps*, LogicLayerCaps*);
void test_tlb_integration(ResourceLayerCaps*);
extern "C" void run_task_selftests(void* resources);
extern "C" void run_process_selftests(void* resources, void* logic);
