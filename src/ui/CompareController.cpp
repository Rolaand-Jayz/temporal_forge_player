// CompareController.cpp
#include "ui/CompareController.hpp"

#include <algorithm>

namespace temporal_forge {

CompareController::CompareController(QObject* parent) : QObject(parent) {}

void CompareController::setActive(bool a) {
    if (a == active_) return;
    active_ = a;
    emit changed();
}

void CompareController::setSplitPosition(float p) {
    p = std::max(0.0f, std::min(1.0f, p));
    if (p == split_) return;
    split_ = p;
    emit changed();
}

} // namespace temporal_forge
