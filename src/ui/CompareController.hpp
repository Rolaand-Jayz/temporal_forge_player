// CompareController.hpp — QML bridge for the split-screen A/B compare mode.
//
// When active, VideoSurfaceItem paints the left half with nearest-neighbor
// scaling (raw/unenhanced) and the right half with smooth scaling (enhanced),
// separated by a draggable divider. This gives an immediate visual "is the
// enhancement doing anything?" answer.
//
// Toggle via the 'C' key. The divider position is draggable (0.0 = all raw,
// 1.0 = all enhanced). Hold Shift+C to invert which side is which.
#pragma once
#include <QObject>

namespace temporal_forge {

class CompareController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY changed)
    Q_PROPERTY(float splitPosition READ splitPosition WRITE setSplitPosition NOTIFY changed)
    Q_PROPERTY(bool rawOnLeft READ rawOnLeft NOTIFY changed)

public:
    explicit CompareController(QObject* parent = nullptr);

    bool active() const { return active_; }
    void setActive(bool a);

    float splitPosition() const { return split_; }
    void setSplitPosition(float p);

    // Which side shows the raw (unenhanced) frame. Default: left.
    bool rawOnLeft() const { return rawOnLeft_; }
    Q_INVOKABLE void swapSides() { rawOnLeft_ = !rawOnLeft_; emit changed(); }

signals:
    void changed();

private:
    bool active_ = false;
    float split_ = 0.5f;   // 0.0 = left edge, 1.0 = right edge
    bool rawOnLeft_ = true;
};

} // namespace temporal_forge
