/*
 * Created: 2026/3/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include <random>
#include <cmath>
#include <vector>
#include <algorithm>

#include "renderer/util/kd_tree.h"

MI_NAMESPACE_BEGIN

// Simple point struct for testing
struct TestPoint {
    float x, y, z;
    
    TestPoint() = default;
    TestPoint(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Point with additional data
struct TestPointWithData {
    float x, y, z;
    int id;
    std::string name;
    
    TestPointWithData() : id(0) {}
    TestPointWithData(float x_, float y_, float z_, int id_, const std::string& name_)
        : x(x_), y(y_), z(z_), id(id_), name(name_) {}
};

MI_NAMESPACE_END

// Test fixture for KD-Tree tests
class KDTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create some test points in a grid pattern
        test_points_ = {
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
            {1.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 1.0f},
            {1.0f, 1.0f, 1.0f},
            {2.0f, 0.0f, 0.0f},
            {0.0f, 2.0f, 0.0f},
            {0.0f, 0.0f, 2.0f},
            {-1.0f, 0.0f, 0.0f},
            {0.0f, -1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f},
        };
    }
    
    std::vector<mi::TestPoint> test_points_;
};

// Test basic construction and empty tree
TEST_F(KDTreeTest, EmptyTree) {
    mi::KDTree3D<mi::TestPoint> tree;
    EXPECT_TRUE(tree.Empty());
    EXPECT_EQ(tree.Size(), 0);
    
    // Queries on empty tree should return empty results
    mi::TestPoint query{0.0f, 0.0f, 0.0f};
    auto nn = tree.NearestNeighbor(query);
    EXPECT_FALSE(nn.has_value());
    
    auto knn = tree.KNN(query, 5);
    EXPECT_TRUE(knn.empty());
    
    auto radius = tree.RadiusSearch(query, 10.0f);
    EXPECT_TRUE(radius.empty());
    
    EXPECT_EQ(tree.CountInRadius(query, 10.0f), 0);
}

// Test building tree and basic queries
TEST_F(KDTreeTest, BuildAndSize) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    EXPECT_FALSE(tree.Empty());
    EXPECT_EQ(tree.Size(), test_points_.size());
}

// Test move construction
TEST_F(KDTreeTest, MoveConstruction) {
    mi::KDTree3D<mi::TestPoint> tree1;
    tree1.Build(test_points_);
    
    auto size = tree1.Size();
    
    mi::KDTree3D<mi::TestPoint> tree2(std::move(tree1));
    EXPECT_EQ(tree2.Size(), size);
    EXPECT_TRUE(tree1.Empty());  // NOLINT(bugprone-use-after-move) - testing moved-from state
}

// Test clearing the tree
TEST_F(KDTreeTest, Clear) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    EXPECT_FALSE(tree.Empty());
    
    tree.Clear();
    EXPECT_TRUE(tree.Empty());
    EXPECT_EQ(tree.Size(), 0);
}

// Test nearest neighbor search
TEST_F(KDTreeTest, NearestNeighbor) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    // Query at origin - should find (0,0,0)
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto nn = tree.NearestNeighbor(query);
        ASSERT_TRUE(nn.has_value());
        // Check point content, not index (index changes during tree construction)
        const auto& pt = tree.GetPoint(nn->point_index);
        EXPECT_FLOAT_EQ(pt.x, 0.0f);
        EXPECT_FLOAT_EQ(pt.y, 0.0f);
        EXPECT_FLOAT_EQ(pt.z, 0.0f);
        EXPECT_FLOAT_EQ(nn->distance_sq, 0.0f);
    }
    
    // Query near (1,1,1) - should find (1,1,1)
    {
        mi::TestPoint query{0.9f, 0.9f, 0.9f};
        auto nn = tree.NearestNeighbor(query);
        ASSERT_TRUE(nn.has_value());
        const auto& pt = tree.GetPoint(nn->point_index);
        EXPECT_FLOAT_EQ(pt.x, 1.0f);
        EXPECT_FLOAT_EQ(pt.y, 1.0f);
        EXPECT_FLOAT_EQ(pt.z, 1.0f);
    }
    
    // Query at (1.5, 0, 0) - should find (2,0,0) or (1,0,0)
    {
        mi::TestPoint query{1.5f, 0.0f, 0.0f};
        auto nn = tree.NearestNeighbor(query);
        ASSERT_TRUE(nn.has_value());
        const auto& pt = tree.GetPoint(nn->point_index);
        // Either (1,0,0) or (2,0,0) is correct (both distance 0.5)
        EXPECT_TRUE((pt.x == 1.0f && pt.y == 0.0f && pt.z == 0.0f) ||
                    (pt.x == 2.0f && pt.y == 0.0f && pt.z == 0.0f));
    }
}

// Test K-nearest neighbors
TEST_F(KDTreeTest, KNN) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    // Query at origin, k=3
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto knn = tree.KNN(query, 3);
        ASSERT_EQ(knn.size(), 3);
        
        // First should be at distance 0
        EXPECT_FLOAT_EQ(knn[0].distance_sq, 0.0f);
        
        // Results should be sorted by distance
        for (size_t i = 1; i < knn.size(); ++i) {
            EXPECT_GE(knn[i].distance_sq, knn[i-1].distance_sq);
        }
    }
    
    // Query at (0.5, 0.5, 0.5), k=4
    {
        mi::TestPoint query{0.5f, 0.5f, 0.5f};
        auto knn = tree.KNN(query, 4);
        ASSERT_EQ(knn.size(), 4);
        
        // All 4 corners of the unit cube should be closest
        // Distance from (0.5, 0.5, 0.5) to any corner is sqrt(0.75)
        for (const auto& result : knn) {
            EXPECT_FLOAT_EQ(result.distance_sq, 0.75f);
        }
    }
    
    // k larger than number of points
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto knn = tree.KNN(query, 1000);
        EXPECT_EQ(knn.size(), test_points_.size());
    }
    
    // k=0 should return empty
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto knn = tree.KNN(query, 0);
        EXPECT_TRUE(knn.empty());
    }
}

// Test radius search
TEST_F(KDTreeTest, RadiusSearch) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    // Query at origin with radius 1.5
    // Points within radius 1.5 from origin:
    // - origin (dist 0): 1 point
    // - {±1,0,0}, {0,±1,0}, {0,0,±1} (dist 1): 6 points  
    // - {1,1,0}, {1,0,1}, {0,1,1} (dist sqrt(2) ≈ 1.41): 3 points
    // Total: 10 points
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto results = tree.RadiusSearch(query, 1.5f);
        
        EXPECT_EQ(results.size(), 10);
        
        // All results should be within radius
        for (const auto& result : results) {
            EXPECT_LE(result.distance_sq, 1.5f * 1.5f);
        }
    }
    
    // Query at origin with radius 0.5
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto results = tree.RadiusSearch(query, 0.5f);
        
        // Should only find the origin
        EXPECT_EQ(results.size(), 1);
        EXPECT_FLOAT_EQ(results[0].distance_sq, 0.0f);
    }
    
    // Query far from all points
    {
        mi::TestPoint query{100.0f, 100.0f, 100.0f};
        auto results = tree.RadiusSearch(query, 1.0f);
        EXPECT_TRUE(results.empty());
    }
    
    // Radius 0 should return empty
    {
        mi::TestPoint query{0.0f, 0.0f, 0.0f};
        auto results = tree.RadiusSearch(query, 0.0f);
        EXPECT_TRUE(results.empty());
    }
}

// Test count in radius
TEST_F(KDTreeTest, CountInRadius) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    mi::TestPoint query{0.0f, 0.0f, 0.0f};
    
    // Count should match RadiusSearch size (10 points within radius 1.5)
    auto radius_results = tree.RadiusSearch(query, 1.5f);
    auto count = tree.CountInRadius(query, 1.5f);
    EXPECT_EQ(static_cast<size_t>(count), radius_results.size());
    EXPECT_EQ(count, 10);
    
    // Zero radius
    EXPECT_EQ(tree.CountInRadius(query, 0.0f), 0);
}

// Test with points containing additional data
TEST_F(KDTreeTest, PointsWithData) {
    std::vector<mi::TestPointWithData> points_with_data;
    for (size_t i = 0; i < test_points_.size(); ++i) {
        const auto& pt = test_points_[i];
        points_with_data.emplace_back(pt.x, pt.y, pt.z, 
                                       static_cast<int>(i), 
                                       "Point_" + std::to_string(i));
    }
    
    mi::KDTree3D<mi::TestPointWithData> tree;
    tree.Build(points_with_data);
    
    mi::TestPointWithData query{0.0f, 0.0f, 0.0f, -1, "Query"};
    auto nn = tree.NearestNeighbor(query);
    
    ASSERT_TRUE(nn.has_value());
    const auto& nearest = tree.GetPoint(nn->point_index);
    // Check that we found the point at origin (id=0, name="Point_0")
    EXPECT_FLOAT_EQ(nearest.x, 0.0f);
    EXPECT_FLOAT_EQ(nearest.y, 0.0f);
    EXPECT_FLOAT_EQ(nearest.z, 0.0f);
    EXPECT_EQ(nearest.id, 0);
    EXPECT_EQ(nearest.name, "Point_0");
}

// Test with random points
TEST_F(KDTreeTest, RandomPoints) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    
    const int num_points = 1000;
    std::vector<mi::TestPoint> random_points;
    random_points.reserve(num_points);
    
    for (int i = 0; i < num_points; ++i) {
        random_points.emplace_back(dist(rng), dist(rng), dist(rng));
    }
    
    // Keep a copy for brute force verification (tree may reorder points)
    std::vector<mi::TestPoint> random_points_copy = random_points;
    
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(std::move(random_points));
    
    EXPECT_EQ(tree.Size(), static_cast<size_t>(num_points));
    
    // Verify nearest neighbor with brute force
    mi::TestPoint query{dist(rng), dist(rng), dist(rng)};
    auto nn = tree.NearestNeighbor(query);
    ASSERT_TRUE(nn.has_value());
    
    // Brute force search on original data
    float min_dist_sq = std::numeric_limits<float>::max();
    for (int i = 0; i < num_points; ++i) {
        float dx = query.x - random_points_copy[i].x;
        float dy = query.y - random_points_copy[i].y;
        float dz = query.z - random_points_copy[i].z;
        float dist_sq = dx*dx + dy*dy + dz*dz;
        if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
        }
    }
    
    // Compare distances, not indices (indices change during tree construction)
    EXPECT_FLOAT_EQ(nn->distance_sq, min_dist_sq);
    
    // Verify the found point has the correct distance
    const auto& found_pt = tree.GetPoint(nn->point_index);
    float verify_dx = query.x - found_pt.x;
    float verify_dy = query.y - found_pt.y;
    float verify_dz = query.z - found_pt.z;
    float verify_dist_sq = verify_dx*verify_dx + verify_dy*verify_dy + verify_dz*verify_dz;
    EXPECT_FLOAT_EQ(verify_dist_sq, min_dist_sq);
}

// Test KNN correctness with random points
TEST_F(KDTreeTest, KNNRandomCorrectness) {
    std::mt19937 rng(123);
    std::uniform_real_distribution<float> dist(-50.0f, 50.0f);
    
    const int num_points = 500;
    std::vector<mi::TestPoint> random_points;
    random_points.reserve(num_points);
    
    for (int i = 0; i < num_points; ++i) {
        random_points.emplace_back(dist(rng), dist(rng), dist(rng));
    }
    
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(random_points);
    
    // Test multiple queries
    for (int q = 0; q < 10; ++q) {
        mi::TestPoint query{dist(rng), dist(rng), dist(rng)};
        int k = 5;
        
        auto knn = tree.KNN(query, k);
        ASSERT_EQ(static_cast<int>(knn.size()), k);
        
        // Brute force KNN
        std::vector<std::pair<float, int>> all_distances;
        for (int i = 0; i < num_points; ++i) {
            float dx = query.x - random_points[i].x;
            float dy = query.y - random_points[i].y;
            float dz = query.z - random_points[i].z;
            float dist_sq = dx*dx + dy*dy + dz*dz;
            all_distances.emplace_back(dist_sq, i);
        }
        
        std::partial_sort(all_distances.begin(), all_distances.begin() + k, 
                         all_distances.end());
        
        // Compare
        for (int i = 0; i < k; ++i) {
            EXPECT_FLOAT_EQ(knn[i].distance_sq, all_distances[i].first);
        }
    }
}

// Test radius search correctness with random points
TEST_F(KDTreeTest, RadiusSearchRandomCorrectness) {
    std::mt19937 rng(456);
    std::uniform_real_distribution<float> dist(-50.0f, 50.0f);
    
    const int num_points = 500;
    std::vector<mi::TestPoint> random_points;
    random_points.reserve(num_points);
    
    for (int i = 0; i < num_points; ++i) {
        random_points.emplace_back(dist(rng), dist(rng), dist(rng));
    }
    
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(random_points);
    
    // Test multiple queries
    for (int q = 0; q < 10; ++q) {
        mi::TestPoint query{dist(rng), dist(rng), dist(rng)};
        float radius = 20.0f;
        
        auto results = tree.RadiusSearch(query, radius);
        
        // Brute force radius search
        int brute_count = 0;
        for (int i = 0; i < num_points; ++i) {
            float dx = query.x - random_points[i].x;
            float dy = query.y - random_points[i].y;
            float dz = query.z - random_points[i].z;
            float dist_sq = dx*dx + dy*dy + dz*dz;
            if (dist_sq <= radius * radius) {
                brute_count++;
            }
        }
        
        EXPECT_EQ(static_cast<int>(results.size()), brute_count);
    }
}

// Test custom distance function
TEST_F(KDTreeTest, CustomDistanceFunction) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    // Custom distance function that returns Manhattan distance
    auto manhattan_distance = [](const mi::TestPoint& a, const mi::TestPoint& b) -> float {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
    };
    
    mi::TestPoint query{0.5f, 0.5f, 0.5f};
    auto nn = tree.NearestNeighbor(query, manhattan_distance);
    
    ASSERT_TRUE(nn.has_value());
    // Note: distance_sq is the square of the custom distance
    // For (0,0,0): Manhattan = 1.5, squared = 2.25
    EXPECT_FLOAT_EQ(nn->distance_sq, 2.25f);
}

// Test GetPoints
TEST_F(KDTreeTest, GetPoints) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    const auto& points = tree.GetPoints();
    EXPECT_EQ(points.size(), test_points_.size());
    
    // Verify all original points are present (though possibly reordered)
    for (const auto& original : test_points_) {
        bool found = false;
        for (const auto& tree_pt : points) {
            if (tree_pt.x == original.x && 
                tree_pt.y == original.y && 
                tree_pt.z == original.z) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Original point (" << original.x << ", " 
                          << original.y << ", " << original.z << ") not found";
    }
}

// Test building with move semantics
TEST_F(KDTreeTest, BuildWithMove) {
    std::vector<mi::TestPoint> points_copy = test_points_;
    
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(std::move(points_copy));
    
    EXPECT_EQ(tree.Size(), test_points_.size());
    
    // Verify tree works correctly
    mi::TestPoint query{0.0f, 0.0f, 0.0f};
    auto nn = tree.NearestNeighbor(query);
    ASSERT_TRUE(nn.has_value());
    EXPECT_FLOAT_EQ(nn->distance_sq, 0.0f);
}

// Test dynamic insertion
TEST_F(KDTreeTest, DynamicInsert) {
    mi::KDTree3D<mi::TestPoint> tree;
    
    // Start empty
    EXPECT_TRUE(tree.Empty());
    EXPECT_EQ(tree.Size(), 0);
    
    // Insert first point
    tree.Insert({0.0f, 0.0f, 0.0f});
    EXPECT_EQ(tree.Size(), 1);
    
    // Insert more points
    for (const auto& pt : test_points_) {
        tree.Insert(pt);
    }
    
    EXPECT_EQ(tree.Size(), test_points_.size() + 1);
    
    // Verify queries work
    mi::TestPoint query{0.0f, 0.0f, 0.0f};
    auto nn = tree.NearestNeighbor(query);
    ASSERT_TRUE(nn.has_value());
    EXPECT_FLOAT_EQ(nn->distance_sq, 0.0f);
}

// Test dynamic removal
TEST_F(KDTreeTest, DynamicRemove) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);
    
    size_t initial_size = tree.Size();
    EXPECT_EQ(initial_size, test_points_.size());
    
    // Find and remove the origin point
    auto nn = tree.NearestNeighbor(mi::TestPoint{0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(nn.has_value());
    
    int node_idx = tree.FindNodeByPointIndex(nn->point_index);
    EXPECT_GE(node_idx, 0);
    
    tree.Remove(node_idx);
    EXPECT_EQ(tree.Size(), initial_size - 1);
    
    // Verify the point is no longer found as nearest
    nn = tree.NearestNeighbor(mi::TestPoint{0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(nn.has_value());
    EXPECT_FLOAT_EQ(nn->distance_sq, 1.0f); // Next closest should be at distance 1
}

// Test insert then remove
TEST_F(KDTreeTest, InsertThenRemove) {
    mi::KDTree3D<mi::TestPoint> tree;
    
    // Insert points
    for (int i = 0; i < 100; ++i) {
        float x = static_cast<float>(i % 10);
        float y = static_cast<float>((i / 10) % 10);
        float z = static_cast<float>(i / 100);
        tree.Insert({x, y, z});
    }
    
    EXPECT_EQ(tree.Size(), 100);
    
    // Remove half of them
    for (int i = 0; i < 50; ++i) {
        auto nn = tree.NearestNeighbor({static_cast<float>(i % 10), 
                                        static_cast<float>((i / 10) % 10), 
                                        static_cast<float>(i / 100)});
        if (nn.has_value()) {
            int node_idx = tree.FindNodeByPointIndex(nn->point_index);
            if (node_idx >= 0) {
                tree.Remove(node_idx);
            }
        }
    }
    
    EXPECT_EQ(tree.Size(), 50);
}

// Test manual subtree rebuild
TEST_F(KDTreeTest, ManualRebuild) {
    mi::KDTree3D<mi::TestPoint> tree;
    
    // Insert points in order (will create unbalanced tree)
    for (int i = 0; i < 50; ++i) {
        tree.Insert({static_cast<float>(i), 0.0f, 0.0f});
    }
    
    EXPECT_EQ(tree.Size(), 50);
    
    // Rebuild entire tree
    tree.RebuildSubtree(-1);
    
    EXPECT_EQ(tree.Size(), 50);
    
    // Verify queries still work
    mi::TestPoint query{25.0f, 0.0f, 0.0f};
    auto nn = tree.NearestNeighbor(query);
    ASSERT_TRUE(nn.has_value());
    EXPECT_FLOAT_EQ(nn->distance_sq, 0.0f);
}

// Test scapegoat balancing with many insertions
TEST_F(KDTreeTest, ScapegoatBalancing) {
    mi::KDTree3D<mi::TestPoint> tree;
    
    // Insert points in sorted order (worst case for unbalanced tree)
    // Use lower alpha for more aggressive balancing
    for (int i = 0; i < 500; ++i) {
        tree.Insert({static_cast<float>(i), 0.0f, 0.0f}, 0.6f);
    }
    
    EXPECT_EQ(tree.Size(), 500);
    
    // Verify all points can be found
    for (int i = 0; i < 500; i += 50) {
        mi::TestPoint query{static_cast<float>(i), 0.0f, 0.0f};
        auto nn = tree.NearestNeighbor(query);
        ASSERT_TRUE(nn.has_value());
        EXPECT_FLOAT_EQ(nn->distance_sq, 0.0f);
    }
}

// Test removal with rebuild
TEST_F(KDTreeTest, RemoveAndRebuild) {
    mi::KDTree3D<mi::TestPoint> tree;
    tree.Build(test_points_);

    // Remove points within radius 1.0 from origin (leaves points at distance >= 1.5)
    // This removes: origin, {±1,0,0}, {0,±1,0}, {0,0,±1} = 7 points
    // Remaining: {1,1,0}, {1,0,1}, {0,1,1}, {1,1,1}, {2,0,0}, {0,2,0}, {0,0,2} = 7 points
    auto results = tree.RadiusSearch(mi::TestPoint{0.0f, 0.0f, 0.0f}, 1.0f);
    for (const auto& result : results) {
        int node_idx = tree.FindNodeByPointIndex(result.point_index);
        if (node_idx >= 0) {
            tree.Remove(node_idx);
        }
    }

    // Rebuild to clean up deleted nodes
    tree.RebuildSubtree(-1);

    // Verify remaining points work
    // The closest remaining point to (5,5,5) should be (1,1,1) at distance sqrt(48) ≈ 6.93
    mi::TestPoint query{5.0f, 5.0f, 5.0f};
    auto nn = tree.NearestNeighbor(query);
    ASSERT_TRUE(nn.has_value());
    // Distance from (5,5,5) to (1,1,1) = sqrt(16+16+16) = sqrt(48)
    EXPECT_FLOAT_EQ(nn->distance_sq, 48.0f);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
