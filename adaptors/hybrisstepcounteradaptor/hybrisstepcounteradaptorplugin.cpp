/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd
**
** Copyright (C) 2016 kimmoli
**
**
** $QT_BEGIN_LICENSE:LGPL$
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "hybrisstepcounteradaptorplugin.h"
#include "hybrisstepcounteradaptor.h"
#include "sensormanager.h"
#include "logging.h"

void HybrisStepCounterAdaptorPlugin::Register(class Loader&)
{
    qCInfo(lcSensorFw) << "registering hybrisstepcounteradaptor";
    SensorManager& sm = SensorManager::instance();
    sm.registerDeviceAdaptor<HybrisStepCounterAdaptor>("stepcounteradaptor");
}
