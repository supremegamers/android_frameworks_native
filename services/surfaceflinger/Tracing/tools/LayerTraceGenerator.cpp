/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <vector>
#include "FrontEnd/LayerCreationArgs.h"
#include "FrontEnd/RequestedLayerState.h"
#include "Tracing/LayerTracing.h"
#include "TransactionState.h"
#include "cutils/properties.h"
#undef LOG_TAG
#define LOG_TAG "LayerTraceGenerator"
//#define LOG_NDEBUG 0

#include <Tracing/TransactionProtoParser.h>
#include <gui/LayerState.h>
#include <log/log.h>
#include <renderengine/ExternalTexture.h>
#include <utils/String16.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "LayerProtoHelper.h"

#include "LayerTraceGenerator.h"

namespace android {
using namespace ftl::flag_operators;

bool LayerTraceGenerator::generate(const proto::TransactionTraceFile& traceFile,
                                   const char* outputLayersTracePath) {
    if (traceFile.entry_size() == 0) {
        ALOGD("Trace file is empty");
        return false;
    }

    TransactionProtoParser parser(std::make_unique<TransactionProtoParser::FlingerDataMapper>());

    // frontend
    frontend::LayerLifecycleManager lifecycleManager;
    frontend::LayerHierarchyBuilder hierarchyBuilder{{}};
    frontend::LayerSnapshotBuilder snapshotBuilder;
    display::DisplayMap<ui::LayerStack, frontend::DisplayInfo> displayInfos;

    renderengine::ShadowSettings globalShadowSettings{.ambientColor = {1, 1, 1, 1}};
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.surface_flinger.supports_background_blur", value, "0");
    bool supportsBlur = atoi(value);

    LayerTracing layerTracing;
    layerTracing.setTraceFlags(LayerTracing::TRACE_INPUT | LayerTracing::TRACE_BUFFERS);
    layerTracing.setBufferSize(512 * 1024 * 1024); // 512MB buffer size
    layerTracing.enable();

    ALOGD("Generating %d transactions...", traceFile.entry_size());
    for (int i = 0; i < traceFile.entry_size(); i++) {
        // parse proto
        proto::TransactionTraceEntry entry = traceFile.entry(i);
        ALOGV("    Entry %04d/%04d for time=%" PRId64 " vsyncid=%" PRId64
              " layers +%d -%d handles -%d transactions=%d",
              i, traceFile.entry_size(), entry.elapsed_realtime_nanos(), entry.vsync_id(),
              entry.added_layers_size(), entry.destroyed_layers_size(),
              entry.destroyed_layer_handles_size(), entry.transactions_size());

        std::vector<std::unique_ptr<frontend::RequestedLayerState>> addedLayers;
        addedLayers.reserve((size_t)entry.added_layers_size());
        for (int j = 0; j < entry.added_layers_size(); j++) {
            LayerCreationArgs args;
            parser.fromProto(entry.added_layers(j), args);
            addedLayers.emplace_back(std::make_unique<frontend::RequestedLayerState>(args));
        }

        std::vector<TransactionState> transactions;
        transactions.reserve((size_t)entry.transactions_size());
        for (int j = 0; j < entry.transactions_size(); j++) {
            // apply transactions
            TransactionState transaction = parser.fromProto(entry.transactions(j));
            transactions.emplace_back(std::move(transaction));
        }

        std::vector<uint32_t> destroyedHandles;
        destroyedHandles.reserve((size_t)entry.destroyed_layer_handles_size());
        for (int j = 0; j < entry.destroyed_layer_handles_size(); j++) {
            destroyedHandles.push_back(entry.destroyed_layer_handles(j));
        }

        bool displayChanged = entry.displays_changed();
        if (displayChanged) {
            parser.fromProto(entry.displays(), displayInfos);
        }

        // apply updates
        lifecycleManager.addLayers(std::move(addedLayers));
        lifecycleManager.applyTransactions(transactions);
        lifecycleManager.onHandlesDestroyed(destroyedHandles, /*ignoreUnknownHandles=*/true);

        if (lifecycleManager.getGlobalChanges().test(
                    frontend::RequestedLayerState::Changes::Hierarchy)) {
            hierarchyBuilder.update(lifecycleManager.getLayers(),
                                    lifecycleManager.getDestroyedLayers());
        }

        frontend::LayerSnapshotBuilder::Args args{.root = hierarchyBuilder.getHierarchy(),
                                                  .layerLifecycleManager = lifecycleManager,
                                                  .displays = displayInfos,
                                                  .displayChanges = displayChanged,
                                                  .globalShadowSettings = globalShadowSettings,
                                                  .supportsBlur = supportsBlur,
                                                  .forceFullDamage = false,
                                                  .supportedLayerGenericMetadata = {},
                                                  .genericLayerMetadataKeyMap = {}};
        snapshotBuilder.update(args);

        bool visibleRegionsDirty = lifecycleManager.getGlobalChanges().any(
                frontend::RequestedLayerState::Changes::VisibleRegion |
                frontend::RequestedLayerState::Changes::Hierarchy |
                frontend::RequestedLayerState::Changes::Visibility);

        ALOGV("    layers:%04zu snapshots:%04zu changes:%s", lifecycleManager.getLayers().size(),
              snapshotBuilder.getSnapshots().size(),
              lifecycleManager.getGlobalChanges().string().c_str());

        lifecycleManager.commitChanges();
        // write layers trace
        auto tracingFlags = LayerTracing::TRACE_INPUT | LayerTracing::TRACE_BUFFERS;
        std::unordered_set<uint64_t> stackIdsToSkip;
        if ((tracingFlags & LayerTracing::TRACE_VIRTUAL_DISPLAYS) == 0) {
            for (const auto& displayInfo : displayInfos) {
                if (displayInfo.second.isVirtual) {
                    stackIdsToSkip.insert(displayInfo.first.id);
                }
            }
        }

        const frontend::LayerHierarchy& root = hierarchyBuilder.getHierarchy();

        LayersProto layersProto;
        for (auto& [child, variant] : root.mChildren) {
            if (variant != frontend::LayerHierarchy::Variant::Attached ||
                stackIdsToSkip.find(child->getLayer()->layerStack.id) != stackIdsToSkip.end()) {
                continue;
            }
            LayerProtoHelper::writeHierarchyToProto(layersProto, *child, snapshotBuilder, {},
                                                    tracingFlags);
        }

        auto displayProtos = LayerProtoHelper::writeDisplayInfoToProto(displayInfos);
        layerTracing.notify(visibleRegionsDirty, entry.elapsed_realtime_nanos(), entry.vsync_id(),
                            &layersProto, {}, &displayProtos);
    }
    layerTracing.disable(outputLayersTracePath);
    ALOGD("End of generating trace file. File written to %s", outputLayersTracePath);
    return true;
}

} // namespace android