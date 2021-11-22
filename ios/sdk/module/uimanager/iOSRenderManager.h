/*!
 * iOS SDK
 *
 * Tencent is pleased to support the open source community by making
 * Hippy available.
 *
 * Copyright (C) 2019 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef iOSRenderManager_h
#define iOSRenderManager_h

#include "dom/render_manager.h"
#include <vector>
#include <memory>
#import "HippyUIManager.h"

class iOSRenderManager : public RenderManager {
public:
    void CreateRenderNode(std::vector<std::shared_ptr<DomNode>>&& nodes) override;
    void UpdateRenderNode(std::vector<std::shared_ptr<DomNode>>&& nodes) override;
    void DeleteRenderNode(std::vector<std::shared_ptr<DomNode>>&& nodes) override;
    void MoveRenderNode(std::vector<int32_t>&& ids,
                        int32_t pid,
                        int32_t id) override;

    void Batch() override;

    void UpdateLayout(std::shared_ptr<LayoutResult> result) override;
    void UpdateLayout(std::unordered_map<LayoutDiffMapKey, float> diff) override;

    void DispatchFunction(int32_t id,
                          const std::string &name,
                          std::unordered_map<std::string, std::shared_ptr<DomValue>> param,
                          DispatchFunctionCallback cb) override;

    void AddTouchEventListener(int32_t id,
                               TouchEvent event,
                               OnTouchEventListener listener) override;
    void RemoveTouchEventListener(int32_t id, TouchEvent event) override;

   protected:
    bool ComputeIsLayoutOnly(std::shared_ptr<DomNode> node) const;

    virtual bool CheckStyleJustLayout(std::shared_ptr<DomNode> node) const;

    virtual bool IsJustLayoutProp(const char *prop_name) const;

    std::shared_ptr<DomNode> GetRenderParent(std::shared_ptr<DomNode> node);

    int32_t CalculateRenderNodeIndex(std::shared_ptr<DomNode> parent,
                                     std::shared_ptr<DomNode> node);
private:
    using RenderManager = hippy::RenderManager;
    using DomNode = hippy::DomNode;

    HippyUIManager *uiManager;    
};

#endif /* iOSRenderManager_h */
