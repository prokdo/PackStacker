#pragma once

#include "gui/pack_controller.hpp"

#include <QtQml/qqmlregistration.h>

struct PackControllerRegistration
{
    Q_GADGET
    QML_FOREIGN(PackStacker::PackController)
    QML_NAMED_ELEMENT(PackController)
    QML_UNCREATABLE("The application supplies the controller")
};
