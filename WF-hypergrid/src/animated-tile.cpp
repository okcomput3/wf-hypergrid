/*
 * Animated Tiling Layout for Wayfire
 * 
 * Inspired by Hyprland's animated tiling system.
 * Windows smoothly animate to their new positions when the layout changes.
 * 
 * Architecture:
 * - TileNode: Binary tree node representing window or split
 * - AnimatedGeometry: Manages smooth position/size transitions
 * - TileTree: Per-workspace layout tree
 * - AnimatedTilePlugin: Main plugin coordinating everything
 */

#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/core.hpp>
#include <wayfire/view.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/signal-provider.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/workarea.hpp>

#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <optional>
#include <algorithm>

namespace animated_tile
{

// ============================================================================
// Bezier Curve (same as Hyprland's implementation)
// ============================================================================

class BezierCurve
{
  public:
    BezierCurve() = default;
    
    BezierCurve(float p1x, float p1y, float p2x, float p2y)
        : m_p1{p1x, p1y}, m_p2{p2x, p2y} {}
    
    float getYForX(float x) const
    {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        
        float t = findTForX(x);
        return computeY(t);
    }
    
  private:
    struct Point { float x, y; };
    Point m_p1{0.0f, 0.0f};
    Point m_p2{1.0f, 1.0f};
    
    float computeX(float t) const
    {
        float mt = 1.0f - t;
        return 3.0f * mt * mt * t * m_p1.x + 
               3.0f * mt * t * t * m_p2.x + 
               t * t * t;
    }
    
    float computeY(float t) const
    {
        float mt = 1.0f - t;
        return 3.0f * mt * mt * t * m_p1.y + 
               3.0f * mt * t * t * m_p2.y + 
               t * t * t;
    }
    
    float findTForX(float x) const
    {
        float t = x;
        for (int i = 0; i < 8; i++)
        {
            float currentX = computeX(t);
            float dx = currentX - x;
            if (std::abs(dx) < 0.0001f) break;
            
            float mt = 1.0f - t;
            float derivative = 3.0f * mt * mt * m_p1.x +
                              6.0f * mt * t * (m_p2.x - m_p1.x) +
                              3.0f * t * t * (1.0f - m_p2.x);
            
            if (std::abs(derivative) < 0.0001f) break;
            
            t -= dx / derivative;
            t = std::clamp(t, 0.0f, 1.0f);
        }
        return t;
    }
};

// ============================================================================
// Animated Variable (like Hyprland's CAnimatedVariable)
// ============================================================================

template<typename T>
class AnimatedVar
{
  public:
    AnimatedVar() = default;
    explicit AnimatedVar(T initial) : m_value(initial), m_start(initial), m_goal(initial) {}
    
    void setConfig(BezierCurve* curve, float durationMs)
    {
        m_curve = curve;
        m_durationMs = durationMs;
    }
    
    void set(T goal, bool animate = true)
    {
        if (!animate || m_durationMs <= 0)
        {
            m_value = goal;
            m_goal = goal;
            m_start = goal;
            m_animating = false;
            return;
        }
        
        m_start = m_value;
        m_goal = goal;
        m_startTime = std::chrono::high_resolution_clock::now();
        m_animating = true;
    }
    
    void warp(T value)
    {
        m_value = value;
        m_goal = value;
        m_start = value;
        m_animating = false;
    }
    
    bool tick()
    {
        if (!m_animating)
            return false;
        
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_startTime).count();
        float progress = std::clamp(elapsed / m_durationMs, 0.0f, 1.0f);
        
        float eased = m_curve ? m_curve->getYForX(progress) : progress;
        m_value = lerp(m_start, m_goal, eased);
        
        if (progress >= 1.0f)
        {
            m_value = m_goal;
            m_animating = false;
            return false;
        }
        
        return true;
    }
    
    T value() const { return m_value; }
    T goal() const { return m_goal; }
    bool isAnimating() const { return m_animating; }
    
  private:
    T m_value{};
    T m_start{};
    T m_goal{};
    BezierCurve* m_curve = nullptr;
    float m_durationMs = 300.0f;
    bool m_animating = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_startTime;
    
    static float lerp(float a, float b, float t) { return a + (b - a) * t; }
    static int lerp(int a, int b, float t) { return static_cast<int>(a + (b - a) * t); }
};

// ============================================================================
// Animated Geometry - position and size with smooth transitions
// ============================================================================

struct AnimatedGeometry
{
    AnimatedVar<int> x{0};
    AnimatedVar<int> y{0};
    AnimatedVar<int> width{100};
    AnimatedVar<int> height{100};
    
    void setConfig(BezierCurve* curve, float durationMs)
    {
        x.setConfig(curve, durationMs);
        y.setConfig(curve, durationMs);
        width.setConfig(curve, durationMs);
        height.setConfig(curve, durationMs);
    }
    
    void setGoal(wf::geometry_t geo, bool animate = true)
    {
        x.set(geo.x, animate);
        y.set(geo.y, animate);
        width.set(geo.width, animate);
        height.set(geo.height, animate);
    }
    
    void warp(wf::geometry_t geo)
    {
        x.warp(geo.x);
        y.warp(geo.y);
        width.warp(geo.width);
        height.warp(geo.height);
    }
    
    bool tick()
    {
        bool a = x.tick();
        bool b = y.tick();
        bool c = width.tick();
        bool d = height.tick();
        return a || b || c || d;
    }
    
    wf::geometry_t current() const
    {
        return {x.value(), y.value(), width.value(), height.value()};
    }
    
    wf::geometry_t goal() const
    {
        return {x.goal(), y.goal(), width.goal(), height.goal()};
    }
    
    bool isAnimating() const
    {
        return x.isAnimating() || y.isAnimating() || 
               width.isAnimating() || height.isAnimating();
    }
};

// ============================================================================
// Split Direction
// ============================================================================

enum class SplitDir
{
    HORIZONTAL,  // Children side by side (left | right)
    VERTICAL     // Children stacked (top / bottom)
};

// ============================================================================
// Tile Node - Binary tree node for tiling layout
// ============================================================================

class TileNode;
using TileNodePtr = std::shared_ptr<TileNode>;
using TileNodeWeak = std::weak_ptr<TileNode>;

class TileNode : public std::enable_shared_from_this<TileNode>
{
  public:
    // Factory methods
    static TileNodePtr createLeaf(wayfire_toplevel_view view)
    {
        auto node = std::make_shared<TileNode>();
        node->m_view = view;
        node->m_isLeaf = true;
        return node;
    }
    
    static TileNodePtr createSplit(SplitDir dir, TileNodePtr left, TileNodePtr right)
    {
        auto node = std::make_shared<TileNode>();
        node->m_isLeaf = false;
        node->m_splitDir = dir;
        node->m_children[0] = left;
        node->m_children[1] = right;
        
        if (left) left->m_parent = node;
        if (right) right->m_parent = node;
        
        return node;
    }
    
    bool isLeaf() const { return m_isLeaf; }
    wayfire_toplevel_view view() const { return m_view; }
    SplitDir splitDir() const { return m_splitDir; }
    
    TileNodePtr child(int idx) const 
    { 
        return (idx >= 0 && idx < 2) ? m_children[idx] : nullptr; 
    }
    
    // Set a child at the given index (0 or 1)
    void setChild(int idx, TileNodePtr newChild)
    {
        if (idx < 0 || idx > 1)
            return;
        
        m_children[idx] = newChild;
        if (newChild)
            newChild->m_parent = weak_from_this();
    }
    
    TileNodePtr parent() const { return m_parent.lock(); }
    
    void setParent(TileNodePtr p) 
    { 
        m_parent = p ? p->weak_from_this() : TileNodeWeak{}; 
    }
    
    void clearParent()
    {
        m_parent.reset();
    }
    
    // Geometry management
    AnimatedGeometry& geometry() { return m_geometry; }
    const AnimatedGeometry& geometry() const { return m_geometry; }
    
    void setConfig(BezierCurve* curve, float durationMs)
    {
        m_geometry.setConfig(curve, durationMs);
    }
    
    // Split ratio (0.0 - 1.0, how much space first child takes)
    float splitRatio() const { return m_splitRatio; }
    void setSplitRatio(float ratio) { m_splitRatio = std::clamp(ratio, 0.1f, 0.9f); }
    
    // Calculate and apply layout recursively
    void applyLayout(wf::geometry_t bounds, int gap, bool animate = true)
    {
        m_geometry.setGoal(bounds, animate);
        
        if (m_isLeaf)
            return;
        
        // Calculate child bounds - properly account for gap
        wf::geometry_t child1Bounds, child2Bounds;
        
        if (m_splitDir == SplitDir::HORIZONTAL)
        {
            // Subtract gap from total, then split the remaining space
            int availableWidth = bounds.width - gap;
            int width1 = static_cast<int>(availableWidth * m_splitRatio);
            int width2 = availableWidth - width1;
            
            child1Bounds = {bounds.x, bounds.y, width1, bounds.height};
            child2Bounds = {bounds.x + width1 + gap, bounds.y, width2, bounds.height};
        }
        else
        {
            // Subtract gap from total, then split the remaining space
            int availableHeight = bounds.height - gap;
            int height1 = static_cast<int>(availableHeight * m_splitRatio);
            int height2 = availableHeight - height1;
            
            child1Bounds = {bounds.x, bounds.y, bounds.width, height1};
            child2Bounds = {bounds.x, bounds.y + height1 + gap, bounds.width, height2};
        }
        
        if (m_children[0])
            m_children[0]->applyLayout(child1Bounds, gap, animate);
        if (m_children[1])
            m_children[1]->applyLayout(child2Bounds, gap, animate);
    }
    
    // Tick animation for this node and all children
    bool tickAnimation()
    {
        bool animating = m_geometry.tick();
        
        if (!m_isLeaf)
        {
            if (m_children[0])
                animating |= m_children[0]->tickAnimation();
            if (m_children[1])
                animating |= m_children[1]->tickAnimation();
        }
        
        return animating;
    }
    
    // Find leaf node containing a specific view
    TileNodePtr findView(wayfire_toplevel_view v)
    {
        if (m_isLeaf)
            return (m_view == v) ? shared_from_this() : nullptr;
        
        if (m_children[0])
        {
            auto found = m_children[0]->findView(v);
            if (found) return found;
        }
        if (m_children[1])
        {
            auto found = m_children[1]->findView(v);
            if (found) return found;
        }
        
        return nullptr;
    }
    
    // Collect all leaf views
    void collectViews(std::vector<wayfire_toplevel_view>& out)
    {
        if (m_isLeaf)
        {
            if (m_view)
                out.push_back(m_view);
        }
        else
        {
            if (m_children[0])
                m_children[0]->collectViews(out);
            if (m_children[1])
                m_children[1]->collectViews(out);
        }
    }
    
    // Count leaves
    int countLeaves() const
    {
        if (m_isLeaf)
            return m_view ? 1 : 0;
        
        int count = 0;
        if (m_children[0])
            count += m_children[0]->countLeaves();
        if (m_children[1])
            count += m_children[1]->countLeaves();
        return count;
    }
    
    // Get which child index this node is in its parent (0 or 1), or -1 if no parent
    int childIndex() const
    {
        auto p = parent();
        if (!p)
            return -1;
        
        if (p->child(0).get() == this)
            return 0;
        if (p->child(1).get() == this)
            return 1;
        
        return -1;
    }
    
  public:
    // Default constructor - needed for make_shared
    TileNode() = default;
    
  private:
    bool m_isLeaf = true;
    wayfire_toplevel_view m_view = nullptr;
    
    SplitDir m_splitDir = SplitDir::HORIZONTAL;
    TileNodePtr m_children[2] = {nullptr, nullptr};
    TileNodeWeak m_parent;
    
    float m_splitRatio = 0.5f;
    AnimatedGeometry m_geometry;
};

// ============================================================================
// Tile Tree - manages layout tree for one workspace
// ============================================================================

class TileTree
{
  public:
    TileTree() = default;
    
    void setConfig(BezierCurve* curve, float durationMs, int gap)
    {
        m_curve = curve;
        m_durationMs = durationMs;
        m_gap = gap;
    }
    
    void setBounds(wf::geometry_t bounds)
    {
        m_bounds = bounds;
    }
    
    // Add a view to the tree using dwindle algorithm
    void addView(wayfire_toplevel_view view, bool animate = true)
    {
        auto newLeaf = TileNode::createLeaf(view);
        newLeaf->setConfig(m_curve, m_durationMs);
        
        if (!m_root)
        {
            // First window - just becomes the root
            m_root = newLeaf;
            newLeaf->geometry().warp(m_bounds);
        }
        else if (m_root->isLeaf())
        {
            // Second window - create split at root level
            // Start with HORIZONTAL split (side by side)
            SplitDir dir = SplitDir::HORIZONTAL;
            
            auto newRoot = TileNode::createSplit(dir, m_root, newLeaf);
            newRoot->setConfig(m_curve, m_durationMs);
            m_root->setParent(newRoot);
            newLeaf->setParent(newRoot);
            
            // Warp new leaf to right half for smooth animation
            wf::geometry_t startGeo = {
                m_bounds.x + m_bounds.width / 2,
                m_bounds.y,
                m_bounds.width / 2,
                m_bounds.height
            };
            newLeaf->geometry().warp(startGeo);
            
            m_root = newRoot;
        }
        else
        {
            // Third+ window: dwindle insertion
            // Find the last (deepest rightmost) leaf and split it
            auto lastLeaf = findLastLeaf(m_root);
            if (lastLeaf)
            {
                insertAtLeaf(lastLeaf, newLeaf);
            }
        }
        
        recalculateLayout(animate);
    }
    
    // Remove a view from the tree
    void removeView(wayfire_toplevel_view view, bool animate = true)
    {
        if (!m_root)
            return;
        
        auto node = m_root->findView(view);
        if (!node)
            return;
        
        auto parent = node->parent();
        if (!parent)
        {
            // This was the only window (root leaf)
            m_root = nullptr;
            return;
        }
        
        // Find sibling (the other child of parent)
        int nodeIdx = node->childIndex();
        int siblingIdx = 1 - nodeIdx;
        TileNodePtr sibling = parent->child(siblingIdx);
        
        auto grandparent = parent->parent();
        if (!grandparent)
        {
            // Parent was root, sibling becomes new root
            m_root = sibling;
            if (sibling)
                sibling->clearParent();
        }
        else
        {
            // Replace parent with sibling in grandparent
            int parentIdx = parent->childIndex();
            grandparent->setChild(parentIdx, sibling);
        }
        
        recalculateLayout(animate);
    }
    
    // Check if tree contains a view
    bool hasView(wayfire_toplevel_view view) const
    {
        if (!m_root)
            return false;
        return const_cast<TileNode*>(m_root.get())->findView(view) != nullptr;
    }
    
    // Tick all animations, returns true if still animating
    bool tickAnimations()
    {
        if (!m_root)
            return false;
        return m_root->tickAnimation();
    }
    
    // Get current geometry for a view (for applying to actual window)
    std::optional<wf::geometry_t> getViewGeometry(wayfire_toplevel_view view) const
    {
        if (!m_root)
            return std::nullopt;
        
        auto node = const_cast<TileNode*>(m_root.get())->findView(view);
        if (!node)
            return std::nullopt;
        
        return node->geometry().current();
    }
    
    // Get goal geometry for a view
    std::optional<wf::geometry_t> getViewGoalGeometry(wayfire_toplevel_view view) const
    {
        if (!m_root)
            return std::nullopt;
        
        auto node = const_cast<TileNode*>(m_root.get())->findView(view);
        if (!node)
            return std::nullopt;
        
        return node->geometry().goal();
    }
    
    // Get all managed views
    std::vector<wayfire_toplevel_view> getViews() const
    {
        std::vector<wayfire_toplevel_view> views;
        if (m_root)
            const_cast<TileNode*>(m_root.get())->collectViews(views);
        return views;
    }
    
    bool isEmpty() const { return !m_root || m_root->countLeaves() == 0; }
    
    void recalculateLayout(bool animate = true)
    {
        if (m_root)
        {
            m_root->applyLayout(m_bounds, m_gap, animate);
        }
    }
    
  private:
    TileNodePtr m_root;
    wf::geometry_t m_bounds{0, 0, 1920, 1080};
    BezierCurve* m_curve = nullptr;
    float m_durationMs = 300.0f;
    int m_gap = 10;
    
    // Find the deepest, rightmost leaf (dwindle style)
    // This traverses always going to the second child (right/bottom)
    TileNodePtr findLastLeaf(TileNodePtr node)
    {
        if (!node)
            return nullptr;
        if (node->isLeaf())
            return node;
        
        // In dwindle, always go to second child first (that's where new windows go)
        if (node->child(1))
        {
            auto found = findLastLeaf(node->child(1));
            if (found)
                return found;
        }
        
        // Fallback to first child
        return findLastLeaf(node->child(0));
    }
    
    // Insert newLeaf by splitting existingLeaf
    // This is the core of dwindle: each new window splits the last one
    void insertAtLeaf(TileNodePtr existingLeaf, TileNodePtr newLeaf)
    {
        auto parent = existingLeaf->parent();
        
        // IMPORTANT: Get the child index BEFORE we create the split,
        // because createSplit will change existingLeaf's parent
        int existingChildIdx = existingLeaf->childIndex();
        
        // Dwindle alternates split direction at each level
        // If parent split horizontally, we split vertically, and vice versa
        SplitDir dir;
        if (parent)
        {
            // Alternate from parent's direction
            dir = (parent->splitDir() == SplitDir::HORIZONTAL) 
                ? SplitDir::VERTICAL 
                : SplitDir::HORIZONTAL;
        }
        else
        {
            // No parent means existingLeaf is root - use aspect ratio
            auto geo = existingLeaf->geometry().goal();
            dir = (geo.width > geo.height) 
                ? SplitDir::HORIZONTAL 
                : SplitDir::VERTICAL;
        }
        
        // Warp new leaf to a reasonable starting position BEFORE creating split
        // This prevents weird scaling animations from {0,0,100,100}
        auto existingGeo = existingLeaf->geometry().goal();
        wf::geometry_t newLeafStart;
        if (dir == SplitDir::HORIZONTAL)
        {
            // New leaf will be on the right
            newLeafStart = {
                existingGeo.x + existingGeo.width / 2,
                existingGeo.y,
                existingGeo.width / 2,
                existingGeo.height
            };
        }
        else
        {
            // New leaf will be on the bottom
            newLeafStart = {
                existingGeo.x,
                existingGeo.y + existingGeo.height / 2,
                existingGeo.width,
                existingGeo.height / 2
            };
        }
        newLeaf->geometry().warp(newLeafStart);
        
        // Create a new split node with existing and new as children
        // Existing goes to child[0] (left/top), new goes to child[1] (right/bottom)
        // NOTE: This changes existingLeaf's parent to newSplit!
        auto newSplit = TileNode::createSplit(dir, existingLeaf, newLeaf);
        newSplit->setConfig(m_curve, m_durationMs);
        
        if (!parent)
        {
            // existingLeaf was the root
            m_root = newSplit;
        }
        else
        {
            // Replace existingLeaf with newSplit in the parent
            // Use the index we saved BEFORE createSplit changed the parent
            parent->setChild(existingChildIdx, newSplit);
        }
        
        // The new split node's geometry will be set during recalculateLayout
    }
};

// ============================================================================
// View Animation Data - stored per-view for managing its animation
// ============================================================================

class ViewAnimData : public wf::custom_data_t
{
  public:
    // The goal geometry the view should animate to
    wf::geometry_t goalGeometry{0, 0, 100, 100};
    
    // Transformer for visual animation during transition
    std::shared_ptr<wf::scene::view_2d_transformer_t> transformer;
    std::string transformerName;
    
    // Whether this view is managed by the tiling system
    bool isTiled = false;
};

// ============================================================================
// Main Plugin
// ============================================================================

class AnimatedTilePlugin : public wf::per_output_plugin_instance_t
{
  public:
    // Configuration
    wf::option_wrapper_t<int> opt_gap{"animated-tile/gap"};
    wf::option_wrapper_t<int> opt_duration{"animated-tile/duration"};
    wf::option_wrapper_t<double> opt_bezier_p1_x{"animated-tile/bezier_p1_x"};
    wf::option_wrapper_t<double> opt_bezier_p1_y{"animated-tile/bezier_p1_y"};
    wf::option_wrapper_t<double> opt_bezier_p2_x{"animated-tile/bezier_p2_x"};
    wf::option_wrapper_t<double> opt_bezier_p2_y{"animated-tile/bezier_p2_y"};
    wf::option_wrapper_t<bool> opt_tile_by_default{"animated-tile/tile_by_default"};
    
    void init() override
    {
        // Setup bezier curve
        updateBezier();
        
        // Get workspace bounds
        updateWorkspaceBounds();
        
        // Configure the tile tree
        m_tree.setConfig(&m_bezier, opt_duration, opt_gap);
        m_tree.setBounds(m_workspaceBounds);
        
        // Connect signals
        output->connect(&on_view_mapped);
        output->connect(&on_view_unmapped);
        output->connect(&on_workarea_changed);
        
        // Start animation tick loop
        m_animationActive = false;
    }
    
    void fini() override
    {
        // Remove all transformers
        for (auto& view : m_tree.getViews())
        {
            removeTransformer(view);
        }
        
        // Stop animation loop
        if (m_animationActive)
        {
            output->render->rem_effect(&m_animationHook);
        }
    }
    
  private:
    BezierCurve m_bezier;
    TileTree m_tree;
    wf::geometry_t m_workspaceBounds;
    bool m_animationActive = false;
    
    wf::effect_hook_t m_animationHook = [this] ()
    {
        tickAnimations();
    };
    
    void updateBezier()
    {
        m_bezier = BezierCurve(
            static_cast<float>(double(opt_bezier_p1_x)),
            static_cast<float>(double(opt_bezier_p1_y)),
            static_cast<float>(double(opt_bezier_p2_x)),
            static_cast<float>(double(opt_bezier_p2_y))
        );
    }
    
    void updateWorkspaceBounds()
    {
        m_workspaceBounds = output->workarea->get_workarea();
    }
    
    // Signal handlers
    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped =
        [this] (wf::view_mapped_signal *ev)
    {
        auto view = wf::toplevel_cast(ev->view);
        if (!view)
            return;
        
        // Only tile toplevel views by default
        if (!opt_tile_by_default)
            return;
        
        tileView(view);
    };
    
    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped =
        [this] (wf::view_unmapped_signal *ev)
    {
        auto view = wf::toplevel_cast(ev->view);
        if (!view)
            return;
        
        if (m_tree.hasView(view))
        {
            untileView(view);
        }
    };
    
    wf::signal::connection_t<wf::workarea_changed_signal> on_workarea_changed =
        [this] (wf::workarea_changed_signal *ev)
    {
        updateWorkspaceBounds();
        m_tree.setBounds(m_workspaceBounds);
        m_tree.recalculateLayout(true);
        startAnimationLoop();
    };
    
    void tileView(wayfire_toplevel_view view)
    {
        // Add to tree
        m_tree.addView(view, true);
        
        // Mark as tiled
        auto data = view->get_data_safe<ViewAnimData>();
        data->isTiled = true;
        
        // Create transformer for animation
        ensureTransformer(view);
        
        // Start animation loop
        startAnimationLoop();
    }
    
    void untileView(wayfire_toplevel_view view)
    {
        // Remove from tree
        m_tree.removeView(view, true);
        
        // Remove transformer
        removeTransformer(view);
        
        // Clear data
        if (view->has_data<ViewAnimData>())
        {
            view->erase_data<ViewAnimData>();
        }
        
        // Continue animation for remaining views
        if (!m_tree.isEmpty())
        {
            startAnimationLoop();
        }
    }
    
    void ensureTransformer(wayfire_toplevel_view view)
    {
        auto data = view->get_data_safe<ViewAnimData>();
        
        if (!data->transformer)
        {
            data->transformerName = "animated-tile-" + 
                std::to_string(reinterpret_cast<uintptr_t>(view.get()));
            data->transformer = std::make_shared<wf::scene::view_2d_transformer_t>(view);
            view->get_transformed_node()->add_transformer(
                data->transformer, wf::TRANSFORMER_2D, data->transformerName);
        }
    }
    
    void removeTransformer(wayfire_toplevel_view view)
    {
        if (!view->has_data<ViewAnimData>())
            return;
        
        auto data = view->get_data<ViewAnimData>();
        if (data->transformer && view->get_transformed_node())
        {
            view->get_transformed_node()->rem_transformer(data->transformerName);
            data->transformer = nullptr;
        }
    }
    
    void startAnimationLoop()
    {
        if (!m_animationActive)
        {
            m_animationActive = true;
            output->render->add_effect(&m_animationHook, wf::OUTPUT_EFFECT_PRE);
        }
        output->render->schedule_redraw();
    }
    
    void stopAnimationLoop()
    {
        if (m_animationActive)
        {
            m_animationActive = false;
            output->render->rem_effect(&m_animationHook);
        }
    }
    
    void tickAnimations()
    {
        bool stillAnimating = m_tree.tickAnimations();
        
        // Apply current animated geometry to each view
        for (auto& view : m_tree.getViews())
        {
            applyAnimatedGeometry(view);
        }
        
        if (!stillAnimating)
        {
            // Animation complete - apply final geometry and remove transformers
            for (auto& view : m_tree.getViews())
            {
                finalizeViewGeometry(view);
            }
            stopAnimationLoop();
        }
        else
        {
            output->render->schedule_redraw();
        }
    }
    
    void applyAnimatedGeometry(wayfire_toplevel_view view)
    {
        auto currentGeo = m_tree.getViewGeometry(view);
        auto goalGeo = m_tree.getViewGoalGeometry(view);
        
        if (!currentGeo || !goalGeo)
            return;
        
        // Safety check for valid geometry
        if (goalGeo->width <= 0 || goalGeo->height <= 0)
            return;
        
        auto data = view->get_data_safe<ViewAnimData>();
        
        // Set the view to its goal size/position immediately
        // But use transformer to show it at animated position
        view->set_geometry(*goalGeo);
        
        if (data->transformer)
        {
            // Scale factor (if animating size)
            float scaleX = static_cast<float>(currentGeo->width) / goalGeo->width;
            float scaleY = static_cast<float>(currentGeo->height) / goalGeo->height;
            
            // Clamp scale to reasonable values
            scaleX = std::clamp(scaleX, 0.1f, 10.0f);
            scaleY = std::clamp(scaleY, 0.1f, 10.0f);
            
            // Calculate offset - need to account for scale when translating
            // The transformer scales around the view's center, so we need to 
            // compensate for the size difference when calculating translation
            float goalCenterX = goalGeo->x + goalGeo->width / 2.0f;
            float goalCenterY = goalGeo->y + goalGeo->height / 2.0f;
            float currentCenterX = currentGeo->x + currentGeo->width / 2.0f;
            float currentCenterY = currentGeo->y + currentGeo->height / 2.0f;
            
            float offsetX = currentCenterX - goalCenterX;
            float offsetY = currentCenterY - goalCenterY;
            
            data->transformer->translation_x = offsetX;
            data->transformer->translation_y = offsetY;
            data->transformer->scale_x = scaleX;
            data->transformer->scale_y = scaleY;
        }
        
        view->damage();
    }
    
    void finalizeViewGeometry(wayfire_toplevel_view view)
    {
        auto goalGeo = m_tree.getViewGoalGeometry(view);
        if (!goalGeo)
            return;
        
        // Set final geometry
        view->set_geometry(*goalGeo);
        
        // Reset transformer
        auto data = view->get_data_safe<ViewAnimData>();
        if (data->transformer)
        {
            data->transformer->translation_x = 0;
            data->transformer->translation_y = 0;
            data->transformer->scale_x = 1.0f;
            data->transformer->scale_y = 1.0f;
        }
        
        view->damage();
    }
};

} // namespace animated_tile

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<animated_tile::AnimatedTilePlugin>);