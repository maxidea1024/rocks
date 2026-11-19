// This file is a C++ re-implementation, written for benchmarking only, of the behavior and data
// structures of BoundsOctree.cs and BoundsOctreeNode.cs from UnityOctree
// (https://github.com/Nition/UnityOctree, commit bbc473571c8a0879024077a928ca658a52ecbd99).
// No original source text was copied. The original license follows.
//
// Copyright (c) 2014, Nition
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "UnityOctreePort.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks::spatialbench::unity {

namespace {

// 원본 BestFitChild·Split·SetValues의 자식 배치다: 자식 i의 중심 = 노드 중심 + (sx, sy, sz) · 1/4
// 변. x는 i가 홀수면 +, y는 i < 4면 +, z는 i가 2·3·6·7이면 +다.
Vec3d ChildOffset(uint32 index, double quarter) noexcept {
    const double x = (index % 2 == 0) ? -quarter : quarter;
    const double y = index > 3 ? -quarter : quarter;
    const double z = (index == 2 || index == 3 || index == 6 || index == 7) ? quarter : -quarter;
    return Vec3d(x, y, z);
}

BoundsOctreeNode** AllocateChildArray(Allocator& allocator) noexcept {
    Result<void*> storage =
        allocator.Allocate(sizeof(BoundsOctreeNode*) * 8, alignof(BoundsOctreeNode*));
    if (!storage) {
        platform::Fatal("UnityOctree port: child array allocation failed");
    }
    BoundsOctreeNode** children = static_cast<BoundsOctreeNode**>(storage.Value());
    for (uint32 index = 0; index < 8; ++index) {
        children[index] = nullptr;
    }
    return children;
}

} // namespace

BoundsOctreeNode::BoundsOctreeNode(Allocator& allocator, double baseLength, double minSize,
                                   double looseness, const Vec3d& center) noexcept
    : allocator_(&allocator) {
    objects_.Bind(allocator);
    SetValues(baseLength, minSize, looseness, center);
}

BoundsOctreeNode::~BoundsOctreeNode() noexcept {
    if (children_ != nullptr) {
        for (uint32 index = 0; index < 8; ++index) {
            if (children_[index] != nullptr) {
                Release(*allocator_, children_[index]);
            }
        }
        (void)allocator_->Deallocate(children_);
    }
    if (childBounds_ != nullptr) {
        (void)allocator_->Deallocate(childBounds_);
    }
}

BoundsOctreeNode* BoundsOctreeNode::Create(Allocator& allocator, double baseLength, double minSize,
                                           double looseness, const Vec3d& center) noexcept {
    Result<void*> storage = allocator.Allocate(sizeof(BoundsOctreeNode), alignof(BoundsOctreeNode));
    if (!storage) {
        platform::Fatal("UnityOctree port: node allocation failed");
    }
    return ConstructAt<BoundsOctreeNode>(storage.Value(), allocator, baseLength, minSize, looseness,
                                         center);
}

void BoundsOctreeNode::Release(Allocator& allocator, BoundsOctreeNode* node) noexcept {
    DestroyAt(node);
    (void)allocator.Deallocate(node);
}

bool BoundsOctreeNode::Add(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept {
    if (!Encapsulates(bounds_, objBounds)) {
        return false;
    }
    SubAdd(obj, filter, objBounds);
    return true;
}

bool BoundsOctreeNode::Remove(uint32 obj) noexcept {
    bool removed = false;
    for (uint32 index = 0; index < objects_.Count(); ++index) {
        if (objects_[index].obj == obj) {
            const OctreeObject value = objects_[index];
            removed = objects_.Remove(value);
            break;
        }
    }
    if (!removed && children_ != nullptr) {
        for (uint32 index = 0; index < 8; ++index) {
            removed = children_[index]->Remove(obj);
            if (removed) {
                break;
            }
        }
    }
    if (removed && children_ != nullptr) {
        if (ShouldMerge()) {
            Merge();
        }
    }
    return removed;
}

bool BoundsOctreeNode::Remove(uint32 obj, const Bounds& objBounds) noexcept {
    if (!Encapsulates(bounds_, objBounds)) {
        return false;
    }
    return SubRemove(obj, objBounds);
}

void BoundsOctreeNode::SetChildren(BoundsOctreeNode** childOctrees) noexcept {
    children_ = childOctrees;
}

BoundsOctreeNode* BoundsOctreeNode::ShrinkIfPossible(double minLength) noexcept {
    if (baseLength_ < 2.0 * minLength) {
        return this;
    }
    if (objects_.Count() == 0 && children_ == nullptr) {
        return this;
    }

    int32 bestFit = -1;
    for (uint32 index = 0; index < objects_.Count(); ++index) {
        const OctreeObject& current = objects_[index];
        const int32 newBestFit = BestFitChild(current.bounds.center);
        if (index == 0 || newBestFit == bestFit) {
            if (Encapsulates(childBounds_[newBestFit], current.bounds)) {
                if (bestFit < 0) {
                    bestFit = newBestFit;
                }
            } else {
                return this;
            }
        } else {
            return this;
        }
    }

    if (children_ != nullptr) {
        bool childHadContent = false;
        for (uint32 index = 0; index < 8; ++index) {
            if (children_[index]->HasAnyObjects()) {
                if (childHadContent) {
                    return this;
                }
                if (bestFit >= 0 && bestFit != static_cast<int32>(index)) {
                    return this;
                }
                childHadContent = true;
                bestFit = static_cast<int32>(index);
            }
        }
    }

    if (children_ == nullptr) {
        const Vec3d newCenter = childBounds_[bestFit].center;
        SetValues(baseLength_ / 2.0, minSize_, looseness_, newCenter);
        return this;
    }

    if (bestFit == -1) {
        return this;
    }

    // 고른 자식을 새 루트로 넘기고 나머지(옛 루트와 빈 형제)를 해제한다(원본은 GC에 맡긴다).
    BoundsOctreeNode* chosen = children_[bestFit];
    children_[bestFit] = nullptr;
    return chosen;
}

int32 BoundsOctreeNode::BestFitChild(const Vec3d& objBoundsCenter) const noexcept {
    return (objBoundsCenter.x <= center_.x ? 0 : 1) + (objBoundsCenter.y >= center_.y ? 0 : 4) +
           (objBoundsCenter.z <= center_.z ? 0 : 2);
}

bool BoundsOctreeNode::HasAnyObjects() const noexcept {
    if (objects_.Count() > 0) {
        return true;
    }
    if (children_ != nullptr) {
        for (uint32 index = 0; index < 8; ++index) {
            if (children_[index]->HasAnyObjects()) {
                return true;
            }
        }
    }
    return false;
}

void BoundsOctreeNode::SetValues(double baseLength, double minSize, double looseness,
                                 const Vec3d& center) noexcept {
    baseLength_ = baseLength;
    minSize_ = minSize;
    looseness_ = looseness;
    center_ = center;
    adjLength_ = looseness_ * baseLength;

    bounds_ = Bounds::FromCenterSize(center_, Vec3d(adjLength_, adjLength_, adjLength_));

    const double quarter = baseLength_ / 4.0;
    const double childActualLength = (baseLength_ / 2.0) * looseness_;
    const Vec3d childActualSize(childActualLength, childActualLength, childActualLength);
    // 원본은 SetValues마다 새 Bounds[8]을 만든다(옛 배열은 GC).
    if (childBounds_ != nullptr) {
        (void)allocator_->Deallocate(childBounds_);
    }
    Result<void*> storage = allocator_->Allocate(sizeof(Bounds) * 8, alignof(Bounds));
    if (!storage) {
        platform::Fatal("UnityOctree port: child bounds allocation failed");
    }
    childBounds_ = static_cast<Bounds*>(storage.Value());
    for (uint32 index = 0; index < 8; ++index) {
        childBounds_[index] =
            Bounds::FromCenterSize(center_ + ChildOffset(index, quarter), childActualSize);
    }
}

void BoundsOctreeNode::SubAdd(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept {
    if (children_ == nullptr) {
        if (objects_.Count() < NumObjectsAllowed || (baseLength_ / 2.0) < minSize_) {
            if (!objects_.Add(OctreeObject{obj, filter, objBounds})) {
                platform::Fatal("UnityOctree port: object list allocation failed");
            }
            return;
        }

        Split();
        // 기존 객체가 새 자식에 들어가면 내린다(뒤에서부터, List.Remove로 지움).
        for (int32 index = static_cast<int32>(objects_.Count()) - 1; index >= 0; --index) {
            const OctreeObject existing = objects_[static_cast<uint32>(index)];
            const int32 bestFitChild = BestFitChild(existing.bounds.center);
            if (Encapsulates(children_[bestFitChild]->bounds_, existing.bounds)) {
                children_[bestFitChild]->SubAdd(existing.obj, existing.filter, existing.bounds);
                (void)objects_.Remove(existing);
            }
        }
    }

    const int32 bestFit = BestFitChild(objBounds.center);
    if (Encapsulates(children_[bestFit]->bounds_, objBounds)) {
        children_[bestFit]->SubAdd(obj, filter, objBounds);
    } else {
        if (!objects_.Add(OctreeObject{obj, filter, objBounds})) {
            platform::Fatal("UnityOctree port: object list allocation failed");
        }
    }
}

bool BoundsOctreeNode::SubRemove(uint32 obj, const Bounds& objBounds) noexcept {
    bool removed = false;
    for (uint32 index = 0; index < objects_.Count(); ++index) {
        if (objects_[index].obj == obj) {
            const OctreeObject value = objects_[index];
            removed = objects_.Remove(value);
            break;
        }
    }
    if (!removed && children_ != nullptr) {
        const int32 bestFitChild = BestFitChild(objBounds.center);
        removed = children_[bestFitChild]->SubRemove(obj, objBounds);
    }
    if (removed && children_ != nullptr) {
        if (ShouldMerge()) {
            Merge();
        }
    }
    return removed;
}

void BoundsOctreeNode::Split() noexcept {
    const double quarter = baseLength_ / 4.0;
    const double newLength = baseLength_ / 2.0;
    children_ = AllocateChildArray(*allocator_);
    for (uint32 index = 0; index < 8; ++index) {
        children_[index] = Create(*allocator_, newLength, minSize_, looseness_,
                                  center_ + ChildOffset(index, quarter));
    }
}

void BoundsOctreeNode::Merge() noexcept {
    for (uint32 index = 0; index < 8; ++index) {
        BoundsOctreeNode* child = children_[index];
        for (int32 position = static_cast<int32>(child->objects_.Count()) - 1; position >= 0;
             --position) {
            if (!objects_.Add(child->objects_[static_cast<uint32>(position)])) {
                platform::Fatal("UnityOctree port: object list allocation failed");
            }
        }
    }
    // 원본은 children = null로 자식(과 그 객체 목록)을 GC에 맡긴다.
    for (uint32 index = 0; index < 8; ++index) {
        Release(*allocator_, children_[index]);
    }
    (void)allocator_->Deallocate(children_);
    children_ = nullptr;
}

bool BoundsOctreeNode::ShouldMerge() const noexcept {
    uint32 totalObjects = objects_.Count();
    if (children_ != nullptr) {
        for (uint32 index = 0; index < 8; ++index) {
            const BoundsOctreeNode* child = children_[index];
            if (child->children_ != nullptr) {
                return false;
            }
            totalObjects += child->objects_.Count();
        }
    }
    return totalObjects <= NumObjectsAllowed;
}

bool BoundsOctreeNode::Encapsulates(const Bounds& outer, const Bounds& inner) noexcept {
    return outer.Contains(inner.Min()) && outer.Contains(inner.Max());
}

void BoundsOctreeNode::Measure(uint32 depth, uint64& nodes, uint32& maxDepth, uint64& reservedBytes,
                               uint32& maxObjects) const noexcept {
    ++nodes;
    maxDepth = depth > maxDepth ? depth : maxDepth;
    reservedBytes += sizeof(BoundsOctreeNode) + sizeof(Bounds) * 8 + objects_.ReservedBytes();
    maxObjects = objects_.Count() > maxObjects ? objects_.Count() : maxObjects;
    if (children_ != nullptr) {
        reservedBytes += sizeof(BoundsOctreeNode*) * 8;
        for (uint32 index = 0; index < 8; ++index) {
            children_[index]->Measure(depth + 1, nodes, maxDepth, reservedBytes, maxObjects);
        }
    }
}

BoundsOctree::BoundsOctree(Allocator& allocator, double initialWorldSize,
                           const Vec3d& initialWorldPos, double minNodeSize,
                           double loosenessValue) noexcept
    : allocator_(&allocator) {
    if (minNodeSize > initialWorldSize) {
        minNodeSize = initialWorldSize;
    }
    initialSize_ = initialWorldSize;
    minSize_ = minNodeSize;
    looseness_ = loosenessValue < 1.0 ? 1.0 : (loosenessValue > 2.0 ? 2.0 : loosenessValue);
    rootNode_ =
        BoundsOctreeNode::Create(allocator, initialSize_, minSize_, looseness_, initialWorldPos);
}

BoundsOctree::~BoundsOctree() noexcept {
    if (rootNode_ != nullptr) {
        BoundsOctreeNode::Release(*allocator_, rootNode_);
    }
}

bool BoundsOctree::Add(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept {
    int32 attempts = 0;
    while (!rootNode_->Add(obj, filter, objBounds)) {
        Grow(objBounds.center - rootNode_->Center());
        if (++attempts > 20) {
            ++addFailures;
            return false;
        }
    }
    ++count_;
    return true;
}

bool BoundsOctree::Remove(uint32 obj) noexcept {
    const bool removed = rootNode_->Remove(obj);
    if (removed) {
        --count_;
        Shrink();
    }
    return removed;
}

bool BoundsOctree::Remove(uint32 obj, const Bounds& objBounds) noexcept {
    const bool removed = rootNode_->Remove(obj, objBounds);
    if (removed) {
        --count_;
        Shrink();
    }
    return removed;
}

void BoundsOctree::Grow(const Vec3d& direction) noexcept {
    ++grows;
    int32 xDirection = direction.x >= 0.0 ? 1 : -1;
    int32 yDirection = direction.y >= 0.0 ? 1 : -1;
    int32 zDirection = direction.z >= 0.0 ? 1 : -1;
    BoundsOctreeNode* oldRoot = rootNode_;
    const double half = rootNode_->BaseLength() / 2.0;
    const double newLength = rootNode_->BaseLength() * 2.0;
    const Vec3d newCenter =
        rootNode_->Center() + Vec3d(xDirection * half, yDirection * half, zDirection * half);

    rootNode_ = BoundsOctreeNode::Create(*allocator_, newLength, minSize_, looseness_, newCenter);

    if (oldRoot->HasAnyObjects()) {
        const int32 rootPos = rootNode_->BestFitChild(oldRoot->Center());
        BoundsOctreeNode** children = AllocateChildArray(*allocator_);
        for (int32 index = 0; index < 8; ++index) {
            if (index == rootPos) {
                children[index] = oldRoot;
            } else {
                xDirection = index % 2 == 0 ? -1 : 1;
                yDirection = index > 3 ? -1 : 1;
                zDirection = (index < 2 || (index > 3 && index < 6)) ? -1 : 1;
                children[index] = BoundsOctreeNode::Create(
                    *allocator_, oldRoot->BaseLength(), minSize_, looseness_,
                    newCenter + Vec3d(xDirection * half, yDirection * half, zDirection * half));
            }
        }
        rootNode_->SetChildren(children);
    } else {
        // 원본은 빈 옛 루트를 버린다(GC).
        BoundsOctreeNode::Release(*allocator_, oldRoot);
    }
}

void BoundsOctree::Shrink() noexcept {
    BoundsOctreeNode* shrunk = rootNode_->ShrinkIfPossible(initialSize_);
    if (shrunk != rootNode_) {
        BoundsOctreeNode::Release(*allocator_, rootNode_);
        rootNode_ = shrunk;
    }
}

} // namespace rocks::spatialbench::unity

namespace rocks::spatialbench {

bool UnityIndex::Create(uint32) noexcept {
    Destroy();
    Result<void*> storage =
        allocator_->Allocate(sizeof(unity::BoundsOctree), alignof(unity::BoundsOctree));
    if (!storage) {
        return false;
    }
    tree_ = ConstructAt<unity::BoundsOctree>(storage.Value(), *allocator_, initialWorldSize_,
                                             Vec3d(0.0, 0.0, 0.0), minNodeSize_, looseness_);
    boundedRemoveFallbacks = 0;
    return true;
}

void UnityIndex::Destroy() noexcept {
    if (tree_ != nullptr) {
        DestroyAt(tree_);
        (void)allocator_->Deallocate(tree_);
        tree_ = nullptr;
    }
}

bool UnityIndex::Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept {
    return tree_->Add(id, filter, unity::Bounds::FromBox(bounds));
}

bool UnityIndex::Remove(uint32 id, const Box<double>& bounds) noexcept {
    if (tree_->Remove(id, unity::Bounds::FromBox(bounds))) {
        return true;
    }
    ++boundedRemoveFallbacks;
    return tree_->Remove(id);
}

bool UnityIndex::RemoveWithoutBounds(uint32 id) noexcept {
    return tree_->Remove(id);
}

bool UnityIndex::Update(uint32 id, const Box<double>& oldBounds,
                        const Box<double>& bounds) noexcept {
    // 원본에 이동 API가 없다: 삭제 후 재삽입.
    if (!Remove(id, oldBounds)) {
        return false;
    }
    return tree_->Add(id, ProxyFilter(id), unity::Bounds::FromBox(bounds));
}

void UnityIndex::Describe(JsonWriter& json) const noexcept {
    uint64 nodes = 0;
    uint32 maxDepth = 0;
    uint64 reservedBytes = 0;
    uint32 maxObjects = 0;
    tree_->Root()->Measure(1, nodes, maxDepth, reservedBytes, maxObjects);
    json.FieldInteger("nodes", nodes);
    json.FieldInteger("maxDepth", maxDepth);
    json.FieldInteger("reservedBytes", reservedBytes);
    json.FieldInteger("maxObjectsPerNode", maxObjects);
    json.Field("rootBaseLength", tree_->Root()->BaseLength());
    json.FieldInteger("count", static_cast<uint64>(tree_->Count()));
    json.FieldInteger("grows", tree_->grows);
    json.FieldInteger("addFailures", tree_->addFailures);
    json.FieldInteger("boundedRemoveFallbacks", boundedRemoveFallbacks);
}

} // namespace rocks::spatialbench
