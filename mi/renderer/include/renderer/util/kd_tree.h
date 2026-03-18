/*
 * Created: 2026/3/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_KD_TREE_H
#define MI_KD_TREE_H

#include <vector>
#include <algorithm>
#include <cmath>
#include <optional>
#include <concepts>
#include <queue>
#include <type_traits>
#include <limits>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <core/common.h>

MI_NAMESPACE_BEGIN

// Concept requiring Point type to have public x, y, z members
template<typename T>
concept CHasXYZ = requires(T p) {
    { p.x } -> std::convertible_to<float>;
    { p.y } -> std::convertible_to<float>;
    { p.z } -> std::convertible_to<float>;
};

// Simple AABB for KD-Tree
struct KDTreeAABB {
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;
    
    KDTreeAABB() 
        : min_x(std::numeric_limits<float>::max())
        , min_y(std::numeric_limits<float>::max())
        , min_z(std::numeric_limits<float>::max())
        , max_x(std::numeric_limits<float>::lowest())
        , max_y(std::numeric_limits<float>::lowest())
        , max_z(std::numeric_limits<float>::lowest()) {}
    
    KDTreeAABB(float minx, float miny, float minz, float maxx, float maxy, float maxz)
        : min_x(minx), min_y(miny), min_z(minz)
        , max_x(maxx), max_y(maxy), max_z(maxz) {}
    
    FORCEINLINE void Encapsulate(float x, float y, float z) {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        min_z = std::min(min_z, z);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        max_z = std::max(max_z, z);
    }
    
    FORCEINLINE void Encapsulate(const KDTreeAABB& other) {
        min_x = std::min(min_x, other.min_x);
        min_y = std::min(min_y, other.min_y);
        min_z = std::min(min_z, other.min_z);
        max_x = std::max(max_x, other.max_x);
        max_y = std::max(max_y, other.max_y);
        max_z = std::max(max_z, other.max_z);
    }
    
    // Squared distance from point to AABB (0 if inside)
    [[nodiscard]] FORCEINLINE float DistanceSqToPoint(float x, float y, float z) const {
        float dx = std::max(0.0f, std::max(min_x - x, x - max_x));
        float dy = std::max(0.0f, std::max(min_y - y, y - max_y));
        float dz = std::max(0.0f, std::max(min_z - z, z - max_z));
        return dx * dx + dy * dy + dz * dz;
    }
    
    [[nodiscard]] FORCEINLINE bool IsValid() const {
        return min_x <= max_x && min_y <= max_y && min_z <= max_z;
    }
};

// 3D KD-Tree with scapegoat tree balancing for dynamic insertions/deletions.
// Thread-safe for read operations. Write operations are not thread-safe.
template <CHasXYZ PointType>
class KDTree3D {
public:
    using Point = PointType;
    using DistanceFunc = float(*)(const Point&, const Point&);

    // Scapegoat tree balance factor (0.5 < alpha < 1.0)
    // Lower alpha = more aggressive rebalancing, shallower tree
    // Higher alpha = less rebalancing, potentially deeper tree
    static constexpr float kDefaultAlpha = 0.75f;

    KDTree3D() = default;
    ~KDTree3D() = default;

    // Non-copyable, movable
    KDTree3D(const KDTree3D&) = delete;
    KDTree3D& operator=(const KDTree3D&) = delete;
    KDTree3D(KDTree3D&&) = default;
    KDTree3D& operator=(KDTree3D&&) = default;

    // Build tree from points. Points are copied internally.
    void Build(const std::vector<Point>& points) {
        Clear();
        if (points.empty()) return;
        
        points_ = points;
        nodes_.reserve(points_.size());
        root_index_ = BuildRecursive(0, static_cast<int>(points_.size()), 0);
        UpdateSubtreeInfo(root_index_);
    }

    // Build tree by moving points.
    void Build(std::vector<Point>&& points) {
        Clear();
        if (points.empty()) return;
        
        points_ = std::move(points);
        nodes_.reserve(points_.size());
        root_index_ = BuildRecursive(0, static_cast<int>(points_.size()), 0);
        UpdateSubtreeInfo(root_index_);
    }

    // Clear all data
    void Clear() {
        points_.clear();
        nodes_.clear();
        free_list_.clear();
        root_index_ = -1;
    }

    // Check if tree is empty
    [[nodiscard]] FORCEINLINE bool Empty() const {
        if (root_index_ < 0) return true;
        return nodes_[root_index_].subtree_size == 0;
    }

    // Get number of active (non-deleted) points in tree
    [[nodiscard]] FORCEINLINE size_t Size() const {
        if (root_index_ < 0) return 0;
        return static_cast<size_t>(nodes_[root_index_].subtree_size);
    }

    // Get total number of nodes (including deleted)
    [[nodiscard]] FORCEINLINE size_t TotalNodes() const {
        return nodes_.size() - free_list_.size();
    }

    // Get point by its internal index
    [[nodiscard]] FORCEINLINE const Point& GetPoint(int index) const {
        return points_[index];
    }

    // Get all points (including deleted ones)
    [[nodiscard]] FORCEINLINE const std::vector<Point>& GetPoints() const {
        return points_;
    }

    // Insert a single point. Returns the node index.
    // May trigger subtree rebalancing if tree becomes unbalanced.
    int Insert(const Point& point, float alpha = kDefaultAlpha) {
        int point_index = static_cast<int>(points_.size());
        points_.push_back(point);

        int node_index = AllocateNode(point_index);
        UpdateNodeAABB(node_index);

        if (root_index_ < 0) {
            root_index_ = node_index;
            UpdateSubtreeInfo(root_index_);
            return node_index;
        }

        int rebalance_root = -1;
        InsertRecursive(root_index_, node_index, 0, alpha, rebalance_root);

        // Rebalance if needed
        if (rebalance_root >= 0) {
            RebuildSubtree(rebalance_root);
            // Node indices are preserved during in-place rebuild, so node_index is still valid.
        }

        return node_index;
    }

    // Remove a point by its node index.
    // Uses lazy deletion; actual removal happens during rebuild.
    void Remove(int node_index) {
        if (node_index < 0 || node_index >= static_cast<int>(nodes_.size())) return;

        Node& node = nodes_[node_index];
        if (node.deleted) return;

        node.deleted = true;

        // Scheme B: subtree_size tracks active (non-deleted) nodes only.
        int current = node_index;
        while (current >= 0) {
            --nodes_[current].subtree_size;
            current = nodes_[current].parent;
        }
    }

    // Result type for KNN queries
    struct KNNResult {
        int point_index;
        float distance_sq;

        [[nodiscard]] FORCEINLINE float Distance() const { return std::sqrt(distance_sq); }
        
        bool operator<(const KNNResult& other) const {
            return distance_sq < other.distance_sq;
        }
    };

    // K-Nearest Neighbors search
    [[nodiscard]] std::vector<KNNResult> KNN(const Point& query, int k, 
        DistanceFunc dist_func = nullptr) const {
        std::vector<KNNResult> results;
        if (Empty() || k <= 0) return results;
        
        auto cmp = [](const KNNResult& a, const KNNResult& b) {
            return a.distance_sq < b.distance_sq;
        };
        std::priority_queue<KNNResult, std::vector<KNNResult>, decltype(cmp)> heap(cmp);
        
        KNNRecursive(root_index_, query, k, heap, dist_func);
        
        results.reserve(heap.size());
        while (!heap.empty()) {
            results.push_back(heap.top());
            heap.pop();
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    // Find single nearest neighbor
    [[nodiscard]] std::optional<KNNResult> NearestNeighbor(const Point& query,
        DistanceFunc dist_func = nullptr) const {
        auto results = KNN(query, 1, dist_func);
        if (results.empty()) return std::nullopt;
        return results[0];
    }

    // Result type for radius queries
    struct RadiusResult {
        int point_index;
        float distance_sq;

        [[nodiscard]] FORCEINLINE float Distance() const { return std::sqrt(distance_sq); }
    };

    // Radius search
    [[nodiscard]] std::vector<RadiusResult> RadiusSearch(const Point& query, float radius,
        DistanceFunc dist_func = nullptr) const {
        std::vector<RadiusResult> results;
        if (Empty() || radius <= 0.0f) return results;
        
        const float radius_sq = radius * radius;
        RadiusSearchRecursive(root_index_, query, radius_sq, results, dist_func);
        return results;
    }

    // Count points within radius
    [[nodiscard]] int CountInRadius(const Point& query, float radius,
        DistanceFunc dist_func = nullptr) const {
        if (Empty() || radius <= 0.0f) return 0;
        
        int count = 0;
        const float radius_sq = radius * radius;
        CountInRadiusRecursive(root_index_, query, radius_sq, count, dist_func);
        return count;
    }

    // Get node index for a point (slow, O(n) search)
    [[nodiscard]] int FindNodeByPointIndex(int point_index) const {
        if (point_index < 0 || point_index >= static_cast<int>(points_.size())) return -1;
        return FindNodeByPointIndexRecursive(root_index_, point_index);
    }

private:
    // Internal node structure
    struct Node {
        int point_index;          // Index into points_ array
        int parent;               // Parent node index, -1 for root
        int left_child;           // Left child index, -1 if none
        int right_child;          // Right child index, -1 if none
        int subtree_size;         // Number of nodes in subtree (including this)
        KDTreeAABB aabb;          // AABB of this subtree
        bool deleted;             // Lazy deletion flag
        
        Node(int idx) 
            : point_index(idx)
            , parent(-1)
            , left_child(-1)
            , right_child(-1)
            , subtree_size(1)
            , deleted(false) {}
    };

    std::vector<Point> points_;
    std::vector<Node> nodes_;
    std::vector<int> free_list_;
    int root_index_ = -1;

    static constexpr bool kHasClusterId = requires(const Point& p) { p.cluster_id; };

    static uint64_t ToClusterId(const Point& point)
        requires requires(const Point& p) { p.cluster_id; } {
        return static_cast<uint64_t>(point.cluster_id);
    }

    // Get coordinate by axis
    [[nodiscard]] FORCEINLINE static float GetCoord(const Point& p, int axis) {
        if (axis == 0) return p.x;
        if (axis == 1) return p.y;
        return p.z;
    }

    // Squared distance between two points
    [[nodiscard]] FORCEINLINE static float DistanceSq(const Point& a, const Point& b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // Note: When using custom distance function, it should return the actual distance,
    // not squared distance. The result will be squared internally for comparison.
    [[nodiscard]] FORCEINLINE static float ComputeDistanceSq(const Point& a, const Point& b,
        DistanceFunc dist_func) {
        if (dist_func) {
            const float dist = dist_func(a, b);
            return dist * dist;
        }
        return DistanceSq(a, b);
    }

    // Allocate a new node
    int AllocateNode(int point_index) {
        int node_index;
        if (!free_list_.empty()) {
            node_index = free_list_.back();
            free_list_.pop_back();
            nodes_[node_index] = Node(point_index);
        } else {
            node_index = static_cast<int>(nodes_.size());
            nodes_.emplace_back(point_index);
        }
        return node_index;
    }

    // Free a node for reuse
    void FreeNode(int node_index) {
        free_list_.push_back(node_index);
    }

    // Update AABB for a single node
    void UpdateNodeAABB(int node_index) {
        if (node_index < 0) return;
        Node& node = nodes_[node_index];
        const Point& pt = points_[node.point_index];
        node.aabb = KDTreeAABB(pt.x, pt.y, pt.z, pt.x, pt.y, pt.z);
    }

    // Update subtree size and AABB recursively
    void UpdateSubtreeInfo(int node_index) {
        if (node_index < 0) return;
        
        Node& node = nodes_[node_index];
        node.subtree_size = node.deleted ? 0 : 1;
        UpdateNodeAABB(node_index);
        
        if (node.left_child >= 0) {
            UpdateSubtreeInfo(node.left_child);
            node.subtree_size += nodes_[node.left_child].subtree_size;
            node.aabb.Encapsulate(nodes_[node.left_child].aabb);
        }
        if (node.right_child >= 0) {
            UpdateSubtreeInfo(node.right_child);
            node.subtree_size += nodes_[node.right_child].subtree_size;
            node.aabb.Encapsulate(nodes_[node.right_child].aabb);
        }
    }

    // Build tree recursively
    int BuildRecursive(int start, int end, int depth) {
        if (start >= end) return -1;

        const int axis = depth % 3;
        const int mid = SelectMedian(start, end, axis);

        const int node_index = AllocateNode(mid);
        Node& node = nodes_[node_index];
        node.deleted = false;  // Ensure deleted flag is reset for reused nodes

        node.left_child = BuildRecursive(start, mid, depth + 1);
        node.right_child = BuildRecursive(mid + 1, end, depth + 1);

        if (node.left_child >= 0) {
            nodes_[node.left_child].parent = node_index;
        }
        if (node.right_child >= 0) {
            nodes_[node.right_child].parent = node_index;
        }

        return node_index;
    }

    // Select median
    int SelectMedian(int start, int end, int axis) {
        const int mid = start + (end - start) / 2;
        
        std::nth_element(
            points_.begin() + start,
            points_.begin() + mid,
            points_.begin() + end,
            [axis](const Point& a, const Point& b) {
                return GetCoord(a, axis) < GetCoord(b, axis);
            }
        );
        
        return mid;
    }

    // Insert recursively - stack naturally maintains path
    // Returns true if subtree needs rebalancing (stores rebalance root in out param)
    bool InsertRecursive(int current, int new_node, int depth, float alpha, int& rebalance_root) {
        if (current < 0) {
            return false;
        }

        const int axis = depth % 3;
        Node& current_node = nodes_[current];
        const Node& new_node_ref = nodes_[new_node];
        const Point& current_pt = points_[current_node.point_index];
        const Point& new_pt = points_[new_node_ref.point_index];

        int& child = (GetCoord(new_pt, axis) < GetCoord(current_pt, axis))
            ? current_node.left_child : current_node.right_child;

        if (child < 0) {
            // Insert as child
            child = new_node;
            nodes_[new_node].parent = current;
            // Update current node's info after direct insertion
            current_node.subtree_size++;
            current_node.aabb.Encapsulate(new_pt.x, new_pt.y, new_pt.z);
        } else {
            // Recurse
            const bool needs_rebalance = InsertRecursive(child, new_node, depth + 1, alpha, rebalance_root);

            // Always update current node's info after recursive insertion
            current_node.subtree_size++;
            current_node.aabb.Encapsulate(new_pt.x, new_pt.y, new_pt.z);

            if (needs_rebalance) {
                // Rebalance was triggered below, propagate up.
                // Note: After rebuild, the subtree AABB is updated by UpdateSubtreeInfo,
                // but current_node.aabb was updated before rebuild. This is fine because
                // the rebalance_root will be rebuilt, making this update harmless.
                return true;
            }
            return false;
        }

        // Check balance condition for scapegoat (only after direct child insertion)
        int left_size = (current_node.left_child >= 0) ? nodes_[current_node.left_child].subtree_size : 0;
        int right_size = (current_node.right_child >= 0) ? nodes_[current_node.right_child].subtree_size : 0;
        int total = current_node.subtree_size;

        if (total > 1 && (left_size > alpha * total || right_size > alpha * total)) {
            // This is the scapegoat
            rebalance_root = current;
            return true;
        }

        return false;
    }

    // Flatten subtree into list of node indices (excluding deleted)
    void FlattenSubtreeNodes(int node_idx, std::vector<int>& node_indices) {
        if (node_idx < 0) return;

        const Node& node = nodes_[node_idx];

        if (!node.deleted) {
            node_indices.push_back(node_idx);
        }

        FlattenSubtreeNodes(node.left_child, node_indices);
        FlattenSubtreeNodes(node.right_child, node_indices);
    }

    // Rebuild subtree in-place, preserving node indices.
    // Returns the new root index (same as subtree_root since we reuse nodes).
    int RebuildSubtreeInPlace(std::vector<int>& node_indices, int start, int end, int depth, int parent) {
        if (start >= end) return -1;

        const int axis = depth % 3;
        const int mid = start + (end - start) / 2;

        // Partial sort by axis coordinate
        std::nth_element(
            node_indices.begin() + start,
            node_indices.begin() + mid,
            node_indices.begin() + end,
            [this, axis](int a, int b) {
                return GetCoord(points_[nodes_[a].point_index], axis) < GetCoord(points_[nodes_[b].point_index], axis);
            }
        );

        // Reuse the node at node_indices[mid] as the root of this subtree
        const int node_index = node_indices[mid];
        Node& node = nodes_[node_index];
        node.parent = parent;
        node.deleted = false;

        // Recursively rebuild children
        node.left_child = RebuildSubtreeInPlace(node_indices, start, mid, depth + 1, node_index);
        node.right_child = RebuildSubtreeInPlace(node_indices, mid + 1, end, depth + 1, node_index);

        return node_index;
    }

    // Free deleted nodes in a subtree that are not in the alive set
    void FreeDeletedNodesInSubtree(int node_idx, const std::unordered_set<int>& alive_set) {
        if (node_idx < 0) return;

        const Node& node = nodes_[node_idx];

        // First recurse to children
        int left = node.left_child;
        int right = node.right_child;

        FreeDeletedNodesInSubtree(left, alive_set);
        FreeDeletedNodesInSubtree(right, alive_set);

        // Then free this node if it's not alive
        if (alive_set.find(node_idx) == alive_set.end()) {
            FreeNode(node_idx);
        }
    }

    // Free all nodes in a subtree (including deleted ones)
    void FreeSubtreeNodes(int node_idx) {
        if (node_idx < 0) return;

        const Node& node = nodes_[node_idx];

        FreeSubtreeNodes(node.left_child);
        FreeSubtreeNodes(node.right_child);
        FreeNode(node_idx);
    }

    // Get depth of a node
    int GetNodeDepth(int node_idx) const {
        int depth = 0;
        int current = node_idx;
        while (current >= 0 && nodes_[current].parent >= 0) {
            ++depth;
            current = nodes_[current].parent;
        }
        return depth;
    }

    // Find node by point index
    int FindNodeByPointIndexRecursive(int node_idx, int point_index) const {
        if (node_idx < 0) return -1;
        
        const Node& node = nodes_[node_idx];
        if (node.point_index == point_index) {
            return node_idx;
        }
        
        int left_result = FindNodeByPointIndexRecursive(node.left_child, point_index);
        if (left_result >= 0) return left_result;
        
        return FindNodeByPointIndexRecursive(node.right_child, point_index);
    }

    // KNN recursive search
    template<typename Compare>
    void KNNRecursive(int node_idx, const Point& query, int k,
        std::priority_queue<KNNResult, std::vector<KNNResult>, Compare>& heap,
        DistanceFunc dist_func) const {

        if (node_idx < 0) return;

        const Node& node = nodes_[node_idx];

        // Skip deleted nodes
        if (!node.deleted) {
            const Point& node_point = points_[node.point_index];
            const float dist_sq = ComputeDistanceSq(query, node_point, dist_func);

            const size_t heap_size = heap.size();
            if (heap_size < static_cast<size_t>(k)) {
                heap.push({node.point_index, dist_sq});
            } else if (dist_sq < heap.top().distance_sq) {
                heap.pop();
                heap.push({node.point_index, dist_sq});
            }
        }

        // Get current max distance (for pruning)
        const float max_dist_sq = heap.size() < static_cast<size_t>(k)
            ? std::numeric_limits<float>::max()
            : heap.top().distance_sq;

        const int left = node.left_child;
        const int right = node.right_child;

        float left_dist_sq = std::numeric_limits<float>::max();
        float right_dist_sq = std::numeric_limits<float>::max();

        if (left >= 0) {
            left_dist_sq = nodes_[left].aabb.DistanceSqToPoint(query.x, query.y, query.z);
        }
        if (right >= 0) {
            right_dist_sq = nodes_[right].aabb.DistanceSqToPoint(query.x, query.y, query.z);
        }

        // Visit closer child first for better pruning
        if (left_dist_sq < right_dist_sq) {
            if (left_dist_sq <= max_dist_sq) {
                KNNRecursive(left, query, k, heap, dist_func);
            }
            // Update max_dist_sq after visiting left child
            const float new_max_sq = heap.size() < static_cast<size_t>(k)
                ? std::numeric_limits<float>::max()
                : heap.top().distance_sq;
            if (right_dist_sq <= new_max_sq) {
                KNNRecursive(right, query, k, heap, dist_func);
            }
        } else {
            if (right_dist_sq <= max_dist_sq) {
                KNNRecursive(right, query, k, heap, dist_func);
            }
            const float new_max_sq = heap.size() < static_cast<size_t>(k)
                ? std::numeric_limits<float>::max()
                : heap.top().distance_sq;
            if (left_dist_sq <= new_max_sq) {
                KNNRecursive(left, query, k, heap, dist_func);
            }
        }
    }

    // Radius search recursive
    void RadiusSearchRecursive(int node_idx, const Point& query, float radius_sq,
        std::vector<RadiusResult>& results, DistanceFunc dist_func) const {
        
        if (node_idx < 0) return;
        
        const Node& node = nodes_[node_idx];
        
        if (node.aabb.DistanceSqToPoint(query.x, query.y, query.z) > radius_sq) {
            return;
        }
        
        if (!node.deleted) {
            const Point& node_point = points_[node.point_index];
            const float dist_sq = ComputeDistanceSq(query, node_point, dist_func);
            if (dist_sq <= radius_sq) {
                results.push_back({node.point_index, dist_sq});
            }
        }
        
        RadiusSearchRecursive(node.left_child, query, radius_sq, results, dist_func);
        RadiusSearchRecursive(node.right_child, query, radius_sq, results, dist_func);
    }

    // Count in radius recursive
    void CountInRadiusRecursive(int node_idx, const Point& query, float radius_sq,
        int& count, DistanceFunc dist_func) const {
        
        if (node_idx < 0) return;
        
        const Node& node = nodes_[node_idx];
        
        if (node.aabb.DistanceSqToPoint(query.x, query.y, query.z) > radius_sq) {
            return;
        }
        
        if (!node.deleted) {
            const Point& node_point = points_[node.point_index];
            const float dist_sq = ComputeDistanceSq(query, node_point, dist_func);
            if (dist_sq <= radius_sq) {
                ++count;
            }
        }
        
        CountInRadiusRecursive(node.left_child, query, radius_sq, count, dist_func);
        CountInRadiusRecursive(node.right_child, query, radius_sq, count, dist_func);
    }

public:

    // Manually rebuild a subtree to restore balance.
    // Pass -1 to rebuild entire tree.
    // Node indices are preserved, only topology is changed.
    void RebuildSubtree(int subtree_root) {
        if (subtree_root < 0) {
            subtree_root = root_index_;
        }
        if (subtree_root < 0) return;

        // Collect alive node indices (not point indices)
        std::vector<int> node_indices;
        FlattenSubtreeNodes(subtree_root, node_indices);

        if (node_indices.empty()) {
            // No alive nodes, clear the subtree
            FreeSubtreeNodes(subtree_root);
            root_index_ = -1;
            return;
        }

        // Get parent info before rebuilding
        int parent = nodes_[subtree_root].parent;
        int depth = GetNodeDepth(subtree_root);

        // Free deleted nodes in the subtree (they won't be reused)
        // First, mark all nodes in subtree, then free the ones not in node_indices
        std::unordered_set<int> alive_set(node_indices.begin(), node_indices.end());
        FreeDeletedNodesInSubtree(subtree_root, alive_set);

        // Rebuild in-place using the alive nodes
        int new_root = RebuildSubtreeInPlace(node_indices, 0, static_cast<int>(node_indices.size()), depth, parent);

        // Update parent link
        if (parent >= 0) {
            if (nodes_[parent].left_child == subtree_root) {
                nodes_[parent].left_child = new_root;
            } else {
                nodes_[parent].right_child = new_root;
            }
        } else {
            root_index_ = new_root;
        }

        // Update subtree info (AABB, subtree_size)
        if (new_root >= 0) {
            UpdateSubtreeInfo(new_root);
        }

    }
};

MI_NAMESPACE_END

#endif // MI_KD_TREE_H
