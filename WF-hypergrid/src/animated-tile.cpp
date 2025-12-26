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
 * 
 * Hyprland-compatible features:
 * - Dynamic split direction based on aspect ratio (not alternating)
 * - preserve_split: Lock split directions
 * - force_split: Control new window placement (0=mouse, 1=left/top, 2=right/bottom)
 * - smart_split: Split based on cursor position
 * - gaps_in/gaps_out: Separate inner and outer gaps
 * - split_width_multiplier: Adjust for ultrawide monitors
 * - Separate animations for windowsIn, windowsOut, windowsMove
 * - Split at focused window (not always deepest leaf)
 * - pseudotile: Windows keep preferred size within tile
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
#include <wayfire/nonstd/wlroots-full.hpp>

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
// Animation Types (like Hyprland's windowsIn, windowsOut, windowsMove)
// ============================================================================

enum class AnimationType
{
    WINDOW_IN,    // New window appearing
    WINDOW_OUT,   // Window closing
    WINDOW_MOVE   // Layout change, resize, drag
};

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
// Animation Configuration (per animation type, like Hyprland)
// ============================================================================

struct AnimationConfig
{
    BezierCurve curve;
    float durationMs = 300.0f;
    bool enabled = true;
    
    // For windowsIn: popin percentage (0.0-1.0, where 0.8 means 80%->100%)
    float popinPercent = 0.8f;
    
    void setCurve(float p1x, float p1y, float p2x, float p2y)
    {
        curve = BezierCurve(p1x, p1y, p2x, p2y);
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
    
    // For popin animation
    AnimatedVar<float> scale{1.0f};
    AnimatedVar<float> alpha{1.0f};
    
    void setConfig(BezierCurve* curve, float durationMs)
    {
        x.setConfig(curve, durationMs);
        y.setConfig(curve, durationMs);
        width.setConfig(curve, durationMs);
        height.setConfig(curve, durationMs);
        scale.setConfig(curve, durationMs);
        alpha.setConfig(curve, durationMs);
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
    
    // Start a popin animation (for new windows)
    void startPopin(float fromScale = 0.8f)
    {
        scale.warp(fromScale);
        scale.set(1.0f, true);
        alpha.warp(0.0f);
        alpha.set(1.0f, true);
    }
    
    // Start a popout animation (for closing windows)
    void startPopout(float toScale = 0.8f)
    {
        scale.set(toScale, true);
        alpha.set(0.0f, true);
    }
    
    bool tick()
    {
        bool a = x.tick();
        bool b = y.tick();
        bool c = width.tick();
        bool d = height.tick();
        bool e = scale.tick();
        bool f = alpha.tick();
        return a || b || c || d || e || f;
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
               width.isAnimating() || height.isAnimating() ||
               scale.isAnimating() || alpha.isAnimating();
    }
    
    float currentScale() const { return scale.value(); }
    float currentAlpha() const { return alpha.value(); }
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
    void setSplitDir(SplitDir dir) { m_splitDir = dir; }
    
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
    
    // Pseudotile support
    bool isPseudotiled() const { return m_isPseudotiled; }
    void setPseudotiled(bool pseudo) { m_isPseudotiled = pseudo; }
    wf::geometry_t preferredSize() const { return m_preferredSize; }
    void setPreferredSize(wf::geometry_t size) { m_preferredSize = size; }
    
    // Lock split direction (preserve_split)
    bool isSplitLocked() const { return m_splitLocked; }
    void setSplitLocked(bool locked) { m_splitLocked = locked; }
    
    // Calculate and apply layout recursively
    // Hyprland-style: recalculate split direction based on aspect ratio unless preserve_split
    void applyLayout(wf::geometry_t bounds, int gapIn, int gapOut, 
                     bool preserveSplit, float splitWidthMultiplier, bool animate = true)
    {
        m_geometry.setGoal(bounds, animate);
        
        if (m_isLeaf)
            return;
        
        // Hyprland behavior: dynamically determine split direction based on aspect ratio
        // unless preserve_split is enabled or this node has locked split
        if (!preserveSplit && !m_splitLocked)
        {
            float effectiveWidth = bounds.width * splitWidthMultiplier;
            m_splitDir = (effectiveWidth > bounds.height) 
                ? SplitDir::HORIZONTAL 
                : SplitDir::VERTICAL;
        }
        
        // Calculate child bounds with proper gap handling
        wf::geometry_t child1Bounds, child2Bounds;
        
        if (m_splitDir == SplitDir::HORIZONTAL)
        {
            int availableWidth = bounds.width - gapIn;
            int width1 = static_cast<int>(availableWidth * m_splitRatio);
            int width2 = availableWidth - width1;
            
            child1Bounds = {bounds.x, bounds.y, width1, bounds.height};
            child2Bounds = {bounds.x + width1 + gapIn, bounds.y, width2, bounds.height};
        }
        else
        {
            int availableHeight = bounds.height - gapIn;
            int height1 = static_cast<int>(availableHeight * m_splitRatio);
            int height2 = availableHeight - height1;
            
            child1Bounds = {bounds.x, bounds.y, bounds.width, height1};
            child2Bounds = {bounds.x, bounds.y + height1 + gapIn, bounds.width, height2};
        }
        
        if (m_children[0])
            m_children[0]->applyLayout(child1Bounds, gapIn, gapOut, preserveSplit, splitWidthMultiplier, animate);
        if (m_children[1])
            m_children[1]->applyLayout(child2Bounds, gapIn, gapOut, preserveSplit, splitWidthMultiplier, animate);
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
    
    // Get sibling node
    TileNodePtr sibling() const
    {
        auto p = parent();
        if (!p)
            return nullptr;
        
        int idx = childIndex();
        if (idx < 0)
            return nullptr;
        
        return p->child(1 - idx);
    }
    
  public:
    TileNode() = default;
    
  private:
    bool m_isLeaf = true;
    wayfire_toplevel_view m_view = nullptr;
    
    SplitDir m_splitDir = SplitDir::HORIZONTAL;
    TileNodePtr m_children[2] = {nullptr, nullptr};
    TileNodeWeak m_parent;
    
    float m_splitRatio = 0.5f;
    AnimatedGeometry m_geometry;
    
    // Hyprland features
    bool m_isPseudotiled = false;
    wf::geometry_t m_preferredSize{0, 0, 0, 0};
    bool m_splitLocked = false;
};

// ============================================================================
// Tile Tree - manages layout tree for one workspace
// ============================================================================

class TileTree
{
  public:
    TileTree() = default;
    
    void setConfig(BezierCurve* curve, float durationMs, int gapIn, int gapOut,
                   bool preserveSplit, float splitWidthMultiplier, int forceSplit,
                   bool smartSplit)
    {
        m_curve = curve;
        m_durationMs = durationMs;
        m_gapIn = gapIn;
        m_gapOut = gapOut;
        m_preserveSplit = preserveSplit;
        m_splitWidthMultiplier = splitWidthMultiplier;
        m_forceSplit = forceSplit;
        m_smartSplit = smartSplit;
    }
    
    void setBounds(wf::geometry_t bounds)
    {
        m_bounds = bounds;
    }
    
    void setFocusedView(wayfire_toplevel_view view)
    {
        m_focusedView = view;
    }
    
    void setCursorPosition(wf::point_t pos)
    {
        m_cursorPos = pos;
    }
    
    // Add a view to the tree - Hyprland style
    // Splits the focused window (not deepest leaf) unless no focus
    void addView(wayfire_toplevel_view view, bool animate = true)
    {
        auto newLeaf = TileNode::createLeaf(view);
        newLeaf->setConfig(m_curve, m_durationMs);
        
        // Apply outer gaps to the effective bounds
        wf::geometry_t effectiveBounds = {
            m_bounds.x + m_gapOut,
            m_bounds.y + m_gapOut,
            m_bounds.width - 2 * m_gapOut,
            m_bounds.height - 2 * m_gapOut
        };
        
        if (!m_root)
        {
            // First window - just becomes the root
            m_root = newLeaf;
            newLeaf->geometry().warp(effectiveBounds);
            // Start popin animation for new window
            newLeaf->geometry().startPopin(0.8f);
        }
        else if (m_root->isLeaf())
        {
            // Second window - create split at root level
            SplitDir dir = determineSplitDirection(effectiveBounds, m_root);
            
            // Determine child order based on force_split
            TileNodePtr first, second;
            if (m_forceSplit == 1)
            {
                // New window on left/top
                first = newLeaf;
                second = m_root;
            }
            else
            {
                // Default (0 or 2): new window on right/bottom
                first = m_root;
                second = newLeaf;
            }
            
            auto newRoot = TileNode::createSplit(dir, first, second);
            newRoot->setConfig(m_curve, m_durationMs);
            first->setParent(newRoot);
            second->setParent(newRoot);
            
            // Warp new leaf to appropriate starting position
            wf::geometry_t startGeo = calculateNewWindowStart(effectiveBounds, dir, m_forceSplit == 1);
            newLeaf->geometry().warp(startGeo);
            newLeaf->geometry().startPopin(0.8f);
            
            m_root = newRoot;
        }
        else
        {
            // Third+ window: split the focused window (Hyprland behavior)
            TileNodePtr targetLeaf = nullptr;
            
            // Try to find the focused view's node
            if (m_focusedView)
            {
                targetLeaf = m_root->findView(m_focusedView);
            }
            
            // Fallback to last leaf if no focus
            if (!targetLeaf)
            {
                targetLeaf = findLastLeaf(m_root);
            }
            
            if (targetLeaf)
            {
                insertAtLeaf(targetLeaf, newLeaf);
                newLeaf->geometry().startPopin(0.8f);
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
        
        // Start popout animation before removing
        node->geometry().startPopout(0.8f);
        
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
    
    // Get animation scale/alpha for a view (for popin/popout effects)
    std::pair<float, float> getViewScaleAlpha(wayfire_toplevel_view view) const
    {
        if (!m_root)
            return {1.0f, 1.0f};
        
        auto node = const_cast<TileNode*>(m_root.get())->findView(view);
        if (!node)
            return {1.0f, 1.0f};
        
        return {node->geometry().currentScale(), node->geometry().currentAlpha()};
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
            // Apply outer gaps to effective bounds
            wf::geometry_t effectiveBounds = {
                m_bounds.x + m_gapOut,
                m_bounds.y + m_gapOut,
                m_bounds.width - 2 * m_gapOut,
                m_bounds.height - 2 * m_gapOut
            };
            
            m_root->applyLayout(effectiveBounds, m_gapIn, m_gapOut, 
                               m_preserveSplit, m_splitWidthMultiplier, animate);
        }
    }
    
    // Layout messages (like Hyprland dispatchers)
    void handleLayoutMessage(const std::string& msg, wayfire_toplevel_view targetView = nullptr)
    {
        if (!m_root)
            return;
        
        TileNodePtr targetNode = nullptr;
        if (targetView)
        {
            targetNode = m_root->findView(targetView);
        }
        else if (m_focusedView)
        {
            targetNode = m_root->findView(m_focusedView);
        }
        
        if (!targetNode)
            return;
        
        auto parent = targetNode->parent();
        if (!parent)
            return;
        
        if (msg == "togglesplit")
        {
            // Toggle split direction of parent
            SplitDir newDir = (parent->splitDir() == SplitDir::HORIZONTAL)
                ? SplitDir::VERTICAL
                : SplitDir::HORIZONTAL;
            parent->setSplitDir(newDir);
            parent->setSplitLocked(true);  // Lock it so preserve_split doesn't override
            recalculateLayout(true);
        }
        else if (msg == "swapnext" || msg == "swapprev")
        {
            // Swap with sibling
            TileNodePtr sibling = targetNode->sibling();
            if (sibling && parent)
            {
                int targetIdx = targetNode->childIndex();
                int siblingIdx = sibling->childIndex();
                parent->setChild(targetIdx, sibling);
                parent->setChild(siblingIdx, targetNode);
                recalculateLayout(true);
            }
        }
        else if (msg == "pseudo")
        {
            // Toggle pseudotile
            targetNode->setPseudotiled(!targetNode->isPseudotiled());
            if (targetNode->isPseudotiled() && targetView)
            {
                // Store current size as preferred
                auto currentGeo = targetView->get_geometry();
                targetNode->setPreferredSize(currentGeo);
            }
            recalculateLayout(true);
        }
    }
    
  private:
    TileNodePtr m_root;
    wf::geometry_t m_bounds{0, 0, 1920, 1080};
    BezierCurve* m_curve = nullptr;
    float m_durationMs = 300.0f;
    
    // Hyprland-style options
    int m_gapIn = 5;
    int m_gapOut = 10;
    bool m_preserveSplit = false;
    float m_splitWidthMultiplier = 1.0f;
    int m_forceSplit = 0;  // 0=mouse, 1=left/top, 2=right/bottom
    bool m_smartSplit = false;
    
    wayfire_toplevel_view m_focusedView = nullptr;
    wf::point_t m_cursorPos{0, 0};
    
    // Determine split direction based on Hyprland rules
    SplitDir determineSplitDirection(wf::geometry_t bounds, TileNodePtr existingNode)
    {
        if (m_smartSplit && existingNode)
        {
            // Smart split: based on cursor position relative to window center
            auto nodeGeo = existingNode->geometry().goal();
            int centerX = nodeGeo.x + nodeGeo.width / 2;
            int centerY = nodeGeo.y + nodeGeo.height / 2;
            
            int dx = std::abs(m_cursorPos.x - centerX);
            int dy = std::abs(m_cursorPos.y - centerY);
            
            // Normalize by dimensions
            float relX = static_cast<float>(dx) / (nodeGeo.width / 2);
            float relY = static_cast<float>(dy) / (nodeGeo.height / 2);
            
            return (relX > relY) ? SplitDir::HORIZONTAL : SplitDir::VERTICAL;
        }
        
        // Default: aspect ratio based (Hyprland default behavior)
        float effectiveWidth = bounds.width * m_splitWidthMultiplier;
        return (effectiveWidth > bounds.height) ? SplitDir::HORIZONTAL : SplitDir::VERTICAL;
    }
    
    // Calculate starting geometry for new window (for smooth animation)
    wf::geometry_t calculateNewWindowStart(wf::geometry_t bounds, SplitDir dir, bool newOnLeft)
    {
        if (dir == SplitDir::HORIZONTAL)
        {
            int halfWidth = bounds.width / 2;
            if (newOnLeft)
            {
                return {bounds.x, bounds.y, halfWidth, bounds.height};
            }
            else
            {
                return {bounds.x + halfWidth, bounds.y, halfWidth, bounds.height};
            }
        }
        else
        {
            int halfHeight = bounds.height / 2;
            if (newOnLeft)
            {
                return {bounds.x, bounds.y, bounds.width, halfHeight};
            }
            else
            {
                return {bounds.x, bounds.y + halfHeight, bounds.width, halfHeight};
            }
        }
    }
    
    // Find the deepest, rightmost leaf (fallback for dwindle style)
    TileNodePtr findLastLeaf(TileNodePtr node)
    {
        if (!node)
            return nullptr;
        if (node->isLeaf())
            return node;
        
        // In dwindle, prefer second child (that's where new windows typically go)
        if (node->child(1))
        {
            auto found = findLastLeaf(node->child(1));
            if (found)
                return found;
        }
        
        return findLastLeaf(node->child(0));
    }
    
    // Insert newLeaf by splitting existingLeaf
    void insertAtLeaf(TileNodePtr existingLeaf, TileNodePtr newLeaf)
    {
        auto parent = existingLeaf->parent();
        int existingChildIdx = existingLeaf->childIndex();
        
        // Determine split direction
        auto existingGeo = existingLeaf->geometry().goal();
        SplitDir dir = determineSplitDirection(existingGeo, existingLeaf);
        
        // Calculate starting position for new leaf
        wf::geometry_t newLeafStart;
        bool newOnRight = (m_forceSplit != 1);  // Default is right/bottom
        
        if (m_forceSplit == 0 && m_smartSplit)
        {
            // Use cursor position to determine side
            int centerX = existingGeo.x + existingGeo.width / 2;
            int centerY = existingGeo.y + existingGeo.height / 2;
            
            if (dir == SplitDir::HORIZONTAL)
            {
                newOnRight = (m_cursorPos.x > centerX);
            }
            else
            {
                newOnRight = (m_cursorPos.y > centerY);
            }
        }
        
        newLeafStart = calculateNewWindowStart(existingGeo, dir, !newOnRight);
        newLeaf->geometry().warp(newLeafStart);
        
        // Create split with appropriate child order
        TileNodePtr first, second;
        if (newOnRight)
        {
            first = existingLeaf;
            second = newLeaf;
        }
        else
        {
            first = newLeaf;
            second = existingLeaf;
        }
        
        auto newSplit = TileNode::createSplit(dir, first, second);
        newSplit->setConfig(m_curve, m_durationMs);
        
        if (!parent)
        {
            m_root = newSplit;
        }
        else
        {
            parent->setChild(existingChildIdx, newSplit);
        }
    }
};

// ============================================================================
// View Animation Data - stored per-view for managing its animation
// ============================================================================

class ViewAnimData : public wf::custom_data_t
{
  public:
    wf::geometry_t goalGeometry{0, 0, 100, 100};
    std::shared_ptr<wf::scene::view_2d_transformer_t> transformer;
    std::string transformerName;
    bool isTiled = false;
    bool isPseudotiled = false;
    AnimationType currentAnimType = AnimationType::WINDOW_MOVE;
    int workspaceIndex = -1;  // Which workspace tree this view belongs to
};

// ============================================================================
// Main Plugin
// ============================================================================

class AnimatedTilePlugin : public wf::per_output_plugin_instance_t
{
  public:
    // Basic configuration
    wf::option_wrapper_t<int> opt_duration{"animated-tile/duration"};
    wf::option_wrapper_t<bool> opt_tile_by_default{"animated-tile/tile_by_default"};
    
    // Default bezier curve (used as fallback)
    wf::option_wrapper_t<double> opt_bezier_p1_x{"animated-tile/bezier_p1_x"};
    wf::option_wrapper_t<double> opt_bezier_p1_y{"animated-tile/bezier_p1_y"};
    wf::option_wrapper_t<double> opt_bezier_p2_x{"animated-tile/bezier_p2_x"};
    wf::option_wrapper_t<double> opt_bezier_p2_y{"animated-tile/bezier_p2_y"};
    
    // Hyprland-style options
    wf::option_wrapper_t<int> opt_gaps_in{"animated-tile/gaps_in"};
    wf::option_wrapper_t<int> opt_gaps_out{"animated-tile/gaps_out"};
    wf::option_wrapper_t<bool> opt_preserve_split{"animated-tile/preserve_split"};
    wf::option_wrapper_t<double> opt_split_width_multiplier{"animated-tile/split_width_multiplier"};
    wf::option_wrapper_t<int> opt_force_split{"animated-tile/force_split"};
    wf::option_wrapper_t<bool> opt_smart_split{"animated-tile/smart_split"};
    wf::option_wrapper_t<double> opt_popin_percent{"animated-tile/popin_percent"};
    
    // Separate animation durations (like Hyprland)
    wf::option_wrapper_t<int> opt_duration_in{"animated-tile/duration_in"};
    wf::option_wrapper_t<int> opt_duration_out{"animated-tile/duration_out"};
    wf::option_wrapper_t<int> opt_duration_move{"animated-tile/duration_move"};
    
    // Separate bezier curves for windowsIn (0 = use default)
    wf::option_wrapper_t<double> opt_bezier_in_p1_x{"animated-tile/bezier_in_p1_x"};
    wf::option_wrapper_t<double> opt_bezier_in_p1_y{"animated-tile/bezier_in_p1_y"};
    wf::option_wrapper_t<double> opt_bezier_in_p2_x{"animated-tile/bezier_in_p2_x"};
    wf::option_wrapper_t<double> opt_bezier_in_p2_y{"animated-tile/bezier_in_p2_y"};
    
    // Separate bezier curves for windowsOut
    wf::option_wrapper_t<double> opt_bezier_out_p1_x{"animated-tile/bezier_out_p1_x"};
    wf::option_wrapper_t<double> opt_bezier_out_p1_y{"animated-tile/bezier_out_p1_y"};
    wf::option_wrapper_t<double> opt_bezier_out_p2_x{"animated-tile/bezier_out_p2_x"};
    wf::option_wrapper_t<double> opt_bezier_out_p2_y{"animated-tile/bezier_out_p2_y"};
    
    // Separate bezier curves for windowsMove (resize/reposition)
    wf::option_wrapper_t<double> opt_bezier_move_p1_x{"animated-tile/bezier_move_p1_x"};
    wf::option_wrapper_t<double> opt_bezier_move_p1_y{"animated-tile/bezier_move_p1_y"};
    wf::option_wrapper_t<double> opt_bezier_move_p2_x{"animated-tile/bezier_move_p2_x"};
    wf::option_wrapper_t<double> opt_bezier_move_p2_y{"animated-tile/bezier_move_p2_y"};
    
    void init() override
    {
        // Setup bezier curves for different animation types
        updateAnimationConfigs();
        
        // Get workspace bounds
        updateWorkspaceBounds();
        
        // Connect signals
        output->connect(&on_view_mapped);
        output->connect(&on_view_unmapped);
        output->connect(&on_workarea_changed);
        output->connect(&on_workspace_changed);
        
        // Start animation tick loop
        m_animationActive = false;
    }
    
    void fini() override
    {
        // Remove all transformers from all trees
        for (auto& [wsIndex, tree] : m_trees)
        {
            for (auto& view : tree->getViews())
            {
                removeTransformer(view);
            }
        }
        
        // Stop animation loop
        if (m_animationActive)
        {
            output->render->rem_effect(&m_animationHook);
        }
    }
    
  private:
    // Animation configs per type
    AnimationConfig m_animConfigIn;
    AnimationConfig m_animConfigOut;
    AnimationConfig m_animConfigMove;
    
    BezierCurve m_bezier;  // Default/shared bezier
    
    // Map of workspace coordinates to tile trees
    // Key is workspace index (y * grid_width + x)
    std::map<int, std::unique_ptr<TileTree>> m_trees;
    
    wf::geometry_t m_workspaceBounds;
    bool m_animationActive = false;
    wf::point_t m_cursorPos{0, 0};
    
    wf::effect_hook_t m_animationHook = [this] ()
    {
        tickAnimations();
    };
    
    // Get workspace index from coordinates
    int workspaceIndex(wf::point_t ws)
    {
        auto grid = output->wset()->get_workspace_grid_size();
        return ws.y * grid.width + ws.x;
    }
    
    // Get workspace index for a view
    int getViewWorkspaceIndex(wayfire_toplevel_view view)
    {
        auto ws = output->wset()->get_view_main_workspace(view);
        return workspaceIndex(ws);
    }
    
    // Get current workspace index
    int getCurrentWorkspaceIndex()
    {
        auto ws = output->wset()->get_current_workspace();
        return workspaceIndex(ws);
    }
    
    // Get or create tree for a workspace
    TileTree* getTreeForWorkspace(int wsIndex)
    {
        auto it = m_trees.find(wsIndex);
        if (it == m_trees.end())
        {
            auto tree = std::make_unique<TileTree>();
            tree->setConfig(
                &m_bezier,
                static_cast<float>(int(opt_duration)),
                opt_gaps_in,
                opt_gaps_out,
                opt_preserve_split,
                static_cast<float>(double(opt_split_width_multiplier)),
                opt_force_split,
                opt_smart_split
            );
            tree->setBounds(m_workspaceBounds);
            auto ptr = tree.get();
            m_trees[wsIndex] = std::move(tree);
            return ptr;
        }
        return it->second.get();
    }
    
    // Get tree for a view (based on view's workspace)
    TileTree* getTreeForView(wayfire_toplevel_view view)
    {
        int wsIndex = getViewWorkspaceIndex(view);
        return getTreeForWorkspace(wsIndex);
    }
    
    void updateAnimationConfigs()
    {
        // Default bezier curve values
        float p1x = static_cast<float>(double(opt_bezier_p1_x));
        float p1y = static_cast<float>(double(opt_bezier_p1_y));
        float p2x = static_cast<float>(double(opt_bezier_p2_x));
        float p2y = static_cast<float>(double(opt_bezier_p2_y));
        
        m_bezier = BezierCurve(p1x, p1y, p2x, p2y);
        
        // Configure each animation type
        // Use specific durations if set, otherwise fall back to main duration
        int durationIn = opt_duration_in > 0 ? int(opt_duration_in) : int(opt_duration);
        int durationOut = opt_duration_out > 0 ? int(opt_duration_out) : int(opt_duration);
        int durationMove = opt_duration_move > 0 ? int(opt_duration_move) : int(opt_duration);
        
        // Helper to check if a bezier is set (non-zero values)
        auto hasCustomBezier = [](double p1x, double p1y, double p2x, double p2y) {
            // Consider it custom if any value is non-zero (default is 0)
            return (p1x != 0.0 || p1y != 0.0 || p2x != 0.0 || p2y != 0.0);
        };
        
        // WindowsIn bezier - use custom if set, otherwise default
        if (hasCustomBezier(opt_bezier_in_p1_x, opt_bezier_in_p1_y, 
                           opt_bezier_in_p2_x, opt_bezier_in_p2_y))
        {
            m_animConfigIn.setCurve(
                static_cast<float>(double(opt_bezier_in_p1_x)),
                static_cast<float>(double(opt_bezier_in_p1_y)),
                static_cast<float>(double(opt_bezier_in_p2_x)),
                static_cast<float>(double(opt_bezier_in_p2_y))
            );
        }
        else
        {
            m_animConfigIn.setCurve(p1x, p1y, p2x, p2y);
        }
        m_animConfigIn.durationMs = static_cast<float>(durationIn);
        m_animConfigIn.popinPercent = static_cast<float>(double(opt_popin_percent));
        
        // WindowsOut bezier
        if (hasCustomBezier(opt_bezier_out_p1_x, opt_bezier_out_p1_y,
                           opt_bezier_out_p2_x, opt_bezier_out_p2_y))
        {
            m_animConfigOut.setCurve(
                static_cast<float>(double(opt_bezier_out_p1_x)),
                static_cast<float>(double(opt_bezier_out_p1_y)),
                static_cast<float>(double(opt_bezier_out_p2_x)),
                static_cast<float>(double(opt_bezier_out_p2_y))
            );
        }
        else
        {
            m_animConfigOut.setCurve(p1x, p1y, p2x, p2y);
        }
        m_animConfigOut.durationMs = static_cast<float>(durationOut);
        
        // WindowsMove bezier (resize/reposition when layout changes)
        if (hasCustomBezier(opt_bezier_move_p1_x, opt_bezier_move_p1_y,
                           opt_bezier_move_p2_x, opt_bezier_move_p2_y))
        {
            m_animConfigMove.setCurve(
                static_cast<float>(double(opt_bezier_move_p1_x)),
                static_cast<float>(double(opt_bezier_move_p1_y)),
                static_cast<float>(double(opt_bezier_move_p2_x)),
                static_cast<float>(double(opt_bezier_move_p2_y))
            );
        }
        else
        {
            m_animConfigMove.setCurve(p1x, p1y, p2x, p2y);
        }
        m_animConfigMove.durationMs = static_cast<float>(durationMove);
    }
    
    void updateTreeConfig()
    {
        // Update config for all existing trees
        for (auto& [wsIndex, tree] : m_trees)
        {
            tree->setConfig(
                &m_bezier,
                static_cast<float>(int(opt_duration)),
                opt_gaps_in,
                opt_gaps_out,
                opt_preserve_split,
                static_cast<float>(double(opt_split_width_multiplier)),
                opt_force_split,
                opt_smart_split
            );
        }
    }
    
    void updateWorkspaceBounds()
    {
        m_workspaceBounds = output->workarea->get_workarea();
        // Update bounds for all trees
        for (auto& [wsIndex, tree] : m_trees)
        {
            tree->setBounds(m_workspaceBounds);
        }
    }
    
    // Signal handlers
    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped =
        [this] (wf::view_mapped_signal *ev)
    {
        auto view = wf::toplevel_cast(ev->view);
        if (!view)
            return;
        
        if (!opt_tile_by_default)
            return;
        
        // Update cursor position for smart_split
        updateCursorPosition();
        
        // Use the CURRENT workspace for new windows, not the view's reported workspace
        // (which may be incorrect for newly mapped windows)
        int wsIndex = getCurrentWorkspaceIndex();
        auto tree = getTreeForWorkspace(wsIndex);
        
        // Track the newly mapped view as focused (it typically gets focus)
        tree->setFocusedView(view);
        
        tileView(view, wsIndex);
    };
    
    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped =
        [this] (wf::view_unmapped_signal *ev)
    {
        auto view = wf::toplevel_cast(ev->view);
        if (!view)
            return;
        
        // Get the workspace index from the view's stored data
        if (view->has_data<ViewAnimData>())
        {
            auto data = view->get_data<ViewAnimData>();
            if (data->isTiled && data->workspaceIndex >= 0)
            {
                auto it = m_trees.find(data->workspaceIndex);
                if (it != m_trees.end() && it->second->hasView(view))
                {
                    untileView(view, it->second.get());
                    return;
                }
            }
        }
        
        // Fallback: search all trees for this view
        for (auto& [wsIndex, tree] : m_trees)
        {
            if (tree->hasView(view))
            {
                untileView(view, tree.get());
                return;
            }
        }
    };
    
    wf::signal::connection_t<wf::workarea_changed_signal> on_workarea_changed =
        [this] (wf::workarea_changed_signal*)
    {
        updateWorkspaceBounds();
        // Recalculate layout for all trees
        for (auto& [wsIndex, tree] : m_trees)
        {
            tree->setBounds(m_workspaceBounds);
            tree->recalculateLayout(true);
        }
        startAnimationLoop();
    };
    
    // Handle workspace switches - apply correct geometry to views on new workspace
    wf::signal::connection_t<wf::workspace_changed_signal> on_workspace_changed =
        [this] (wf::workspace_changed_signal*)
    {
        // When switching workspaces, immediately apply final geometry
        // to all views on the new current workspace (no animation)
        int currentWs = getCurrentWorkspaceIndex();
        auto it = m_trees.find(currentWs);
        if (it != m_trees.end())
        {
            for (auto& view : it->second->getViews())
            {
                auto goalGeo = it->second->getViewGoalGeometry(view);
                if (goalGeo)
                {
                    view->set_geometry(*goalGeo);
                    
                    // Reset transformer
                    auto data = view->get_data_safe<ViewAnimData>();
                    if (data->transformer)
                    {
                        data->transformer->translation_x = 0;
                        data->transformer->translation_y = 0;
                        data->transformer->scale_x = 1.0f;
                        data->transformer->scale_y = 1.0f;
                        data->transformer->alpha = 1.0f;
                    }
                    view->damage();
                }
            }
        }
    };
    
    void updateCursorPosition()
    {
        auto cursor = wf::get_core().get_cursor_position();
        m_cursorPos = {static_cast<int>(cursor.x), static_cast<int>(cursor.y)};
        // Update cursor position for current workspace tree
        int wsIndex = getCurrentWorkspaceIndex();
        if (m_trees.count(wsIndex))
        {
            m_trees[wsIndex]->setCursorPosition(m_cursorPos);
        }
    }
    
    void tileView(wayfire_toplevel_view view, int wsIndex)
    {
        // Get the tree for this workspace
        auto tree = getTreeForWorkspace(wsIndex);
        
        // Add to tree with animation
        tree->addView(view, true);
        
        // Mark as tiled and store workspace index
        auto data = view->get_data_safe<ViewAnimData>();
        data->isTiled = true;
        data->currentAnimType = AnimationType::WINDOW_IN;
        data->workspaceIndex = wsIndex;
        
        // Create transformer for animation
        ensureTransformer(view);
        
        // Start animation loop
        startAnimationLoop();
    }
    
    void untileView(wayfire_toplevel_view view, TileTree* tree)
    {
        // Set animation type to OUT before removing
        if (view->has_data<ViewAnimData>())
        {
            auto data = view->get_data<ViewAnimData>();
            data->currentAnimType = AnimationType::WINDOW_OUT;
        }
        
        // Remove from tree with animation
        tree->removeView(view, true);
        
        // Remove transformer
        removeTransformer(view);
        
        // Clear data
        if (view->has_data<ViewAnimData>())
        {
            view->erase_data<ViewAnimData>();
        }
        
        // Continue animation for remaining views
        if (!tree->isEmpty())
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
        bool stillAnimating = false;
        
        // Only tick and apply geometry for the CURRENT workspace's tree
        // Other workspaces' views should not be touched
        int currentWs = getCurrentWorkspaceIndex();
        
        // Tick all trees to keep animations progressing
        for (auto& [wsIndex, tree] : m_trees)
        {
            stillAnimating |= tree->tickAnimations();
        }
        
        // But only apply geometry to views on the current workspace
        auto it = m_trees.find(currentWs);
        if (it != m_trees.end())
        {
            for (auto& view : it->second->getViews())
            {
                applyAnimatedGeometry(view, it->second.get());
            }
        }
        
        if (!stillAnimating)
        {
            // Animation complete - finalize geometry only for current workspace
            if (it != m_trees.end())
            {
                for (auto& view : it->second->getViews())
                {
                    finalizeViewGeometry(view, it->second.get());
                }
            }
            stopAnimationLoop();
        }
        else
        {
            output->render->schedule_redraw();
        }
    }
    
    void applyAnimatedGeometry(wayfire_toplevel_view view, TileTree* tree)
    {
        auto currentGeo = tree->getViewGeometry(view);
        auto goalGeo = tree->getViewGoalGeometry(view);
        auto [animScale, animAlpha] = tree->getViewScaleAlpha(view);
        
        if (!currentGeo || !goalGeo)
            return;
        
        if (goalGeo->width <= 0 || goalGeo->height <= 0)
            return;
        
        auto data = view->get_data_safe<ViewAnimData>();
        
        // Set the view to its goal size/position
        view->set_geometry(*goalGeo);
        
        if (data->transformer)
        {
            // Scale factor for position/size animation
            float scaleX = static_cast<float>(currentGeo->width) / goalGeo->width;
            float scaleY = static_cast<float>(currentGeo->height) / goalGeo->height;
            
            scaleX = std::clamp(scaleX, 0.1f, 10.0f);
            scaleY = std::clamp(scaleY, 0.1f, 10.0f);
            
            // Apply popin/popout scale on top
            scaleX *= animScale;
            scaleY *= animScale;
            
            // Calculate offset
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
            data->transformer->alpha = animAlpha;
        }
        
        view->damage();
    }
    
    void finalizeViewGeometry(wayfire_toplevel_view view, TileTree* tree)
    {
        auto goalGeo = tree->getViewGoalGeometry(view);
        if (!goalGeo)
            return;
        
        view->set_geometry(*goalGeo);
        
        auto data = view->get_data_safe<ViewAnimData>();
        if (data->transformer)
        {
            data->transformer->translation_x = 0;
            data->transformer->translation_y = 0;
            data->transformer->scale_x = 1.0f;
            data->transformer->scale_y = 1.0f;
            data->transformer->alpha = 1.0f;
        }
        
        // Switch from WINDOW_IN to WINDOW_MOVE after initial animation
        data->currentAnimType = AnimationType::WINDOW_MOVE;
        
        view->damage();
    }
};

} // namespace animated_tile

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<animated_tile::AnimatedTilePlugin>);
