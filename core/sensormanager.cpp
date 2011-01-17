/**
   @file sensormanager.cpp
   @brief SensorManager

   <p>
   Copyright (C) 2009-2010 Nokia Corporation

   @author Semi Malinen <semi.malinen@nokia.com
   @author Joep van Gassel <joep.van.gassel@nokia.com>
   @author Timo Rongas <ext-timo.2.rongas@nokia.com>
   @author Ustun Ergenoglu <ext-ustun.ergenoglu@nokia.com>
   @author Lihan Guo <lihan.guo@digia.com>

   This file is part of Sensord.

   Sensord is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Sensord is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Sensord.  If not, see <http://www.gnu.org/licenses/>.
   </p>
 */

#include "sensormanager_a.h"
#include "serviceinfo.h"
#include "sensormanager.h"
#include "loader.h"
#include "idutils.h"
#include "logging.h"
#include "mcewatcher.h"
#include "calibrationhandler.h"

#include <QSocketNotifier>
#include <errno.h>
#include "sockethandler.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>


SensorManager* SensorManager::instance_ = NULL;
int SensorManager::sessionIdCount_ = 0;

inline
QDBusConnection bus()
{
    return QDBusConnection::systemBus();
}

SensorManager& SensorManager::instance()
{
    if ( !instance_ )
    {
        instance_ = new SensorManager;
    }

    return *instance_;
}

SensorManager::SensorManager()
    : errorCode_(SmNoError)
{
    const char* SOCKET_NAME = "/tmp/sensord.sock";

    new SensorManagerAdaptor(this);

    socketHandler_ = new SocketHandler(this);
    connect(socketHandler_, SIGNAL(lostSession(int)), this, SLOT(lostClient(int)));

    Q_ASSERT(socketHandler_->listen(SOCKET_NAME));

    if (chmod(SOCKET_NAME, S_IRWXU|S_IRWXG|S_IRWXO) != 0) {
        sensordLogW() << "Error setting socket permissions! " << SOCKET_NAME;
    }

    displayState_ = true;
    psmState_ = false;

#ifdef SENSORFW_MCE_WATCHER

    mceWatcher_ = new MceWatcher(this);
    connect(mceWatcher_, SIGNAL(displayStateChanged(const bool)),
            this, SLOT(displayStateChanged(const bool)));

    connect(mceWatcher_, SIGNAL(devicePSMStateChanged(const bool)),
            this, SLOT(devicePSMStateChanged(const bool)));


#endif //SENSORFW_MCE_WATCHER
}

SensorManager::~SensorManager()
{
    foreach (const QString& key, sensorInstanceMap_.keys())
    {
         sensordLogW() << "ERROR: sensor" << key << "not released!";
         Q_ASSERT( sensorInstanceMap_[key].sensor_ == 0 );
    }

    foreach (const QString& key, deviceAdaptorInstanceMap_.keys())
    {
         sensordLogW() << "ERROR: device adaptor" << key << "not released!";
         Q_ASSERT( deviceAdaptorInstanceMap_[key].adaptor_ == 0 );
    }

    if (socketHandler_) {
        delete socketHandler_;
    }

#ifdef SENSORFW_MCE_WATCHER
    delete mceWatcher_;
#endif //SENSORFW_MCE_WATCHER
}

void SensorManager::setError(SensorManagerError errorCode, const QString& errorString)
{
    sensordLogW() << "SensorManagerError:" <<  errorString;

    errorCode_   = errorCode;
    errorString_ = errorString;

    emit errorSignal(errorCode);
}

bool SensorManager::registerService()
{
    clearError();

    bool ok = bus().isConnected();
    if ( !ok )
    {
        QDBusError error = bus().lastError();
        setError(SmNotConnected, error.message());
        return false;
    }

    ok = bus().registerObject( OBJECT_PATH, this );
    if ( !ok )
    {
        QDBusError error = bus().lastError();
        setError(SmCanNotRegisterObject, error.message());
        return false;
    }

    ok = bus().registerService ( SERVICE_NAME );
    if ( !ok )
    {
        QDBusError error = bus().lastError();
        setError(SmCanNotRegisterService, error.message());
        return false;
    }
    return true;
}

AbstractSensorChannel* SensorManager::addSensor(const QString& id)
{
    clearError();

    QString cleanId = getCleanId(id);
    QMap<QString, SensorInstanceEntry>::iterator entryIt = sensorInstanceMap_.find(cleanId);

    if (entryIt == sensorInstanceMap_.end()) {
        sensordLogC() << QString("<%1> Sensor not present...").arg(cleanId);
        setError( SmIdNotRegistered, QString(tr("instance for sensor type '%1' not registered").arg(cleanId)) );
        return NULL;
    }

    QString typeName = entryIt.value().type_;

    if ( !sensorFactoryMap_.contains(typeName) )
    {
        setError( SmFactoryNotRegistered, QString(tr("factory for sensor type '%1' not registered").arg(typeName)) );
        return NULL;
    }

    //sensordLogD() << "Creating sensor type:" << typeName << "id:" << id;
    AbstractSensorChannel* sensorChannel = sensorFactoryMap_[typeName](id);
    if ( !sensorChannel->isValid() )
    {
        delete sensorChannel;
        return NULL;
    }
    Q_ASSERT( entryIt.value().sensor_ == 0 );
    entryIt.value().sensor_ = sensorChannel;

    bool ok = bus().registerObject(OBJECT_PATH + "/" + sensorChannel->id(), sensorChannel);
    if ( !ok )
    {
        QDBusError error = bus().lastError();
        setError(SmCanNotRegisterObject, error.message());
        Q_ASSERT ( false ); // TODO: release the sensor and update administration accordingly...
        return NULL;
    }
    return sensorChannel;
}

void SensorManager::removeSensor(const QString& id)
{
    QMap<QString, SensorInstanceEntry>::iterator entryIt = sensorInstanceMap_.find(id);
    bus().unregisterObject(OBJECT_PATH + "/" + id);
    delete entryIt.value().sensor_;
    entryIt.value().sensor_ = 0;
}

bool SensorManager::loadPlugin(const QString& name)
{
    QString errorMessage;
    bool result;

    Loader& l = Loader::instance();
    if (! (result = l.loadPlugin(name, &errorMessage))) {
        setError (SmCanNotRegisterObject, errorMessage);
    }
    return result;
}

int SensorManager::requestSensor(const QString& id)
{
    clearError();

    QString cleanId = getCleanId(id);
    QMap<QString, SensorInstanceEntry>::iterator entryIt = sensorInstanceMap_.find(cleanId);

    if ( entryIt == sensorInstanceMap_.end() )
    {
        setError(SmIdNotRegistered, QString(tr("requested sensor id '%1' not registered")).arg(cleanId));
        return INVALID_SESSION;
    }

    int sessionId = createNewSessionId();
    if(entryIt.value().sessions_.empty())
    {
        AbstractSensorChannel* sensor = addSensor(id);
        if ( sensor == NULL )
        {
            return INVALID_SESSION;
        }
    }
    entryIt.value().sessions_.append(sessionId);

    return sessionId;
}

bool SensorManager::releaseSensor(const QString& id, int sessionId)
{
    Q_ASSERT( !id.contains(';') ); // no parameter passing in release

    clearError();
    QMap<QString, SensorInstanceEntry>::iterator entryIt = sensorInstanceMap_.find(id);

    if ( entryIt == sensorInstanceMap_.end() )
    {
        setError( SmIdNotRegistered, QString(tr("requested sensor id '%1' not registered").arg(id)) );
        return false;
    }

    /// Remove any property requests by this session
    entryIt.value().sensor_->setStandbyOverrideRequest(sessionId, false);
    entryIt.value().sensor_->removeIntervalRequest(sessionId);
    entryIt.value().sensor_->removeDataRangeRequest(sessionId);

    if (entryIt.value().sessions_.empty())
    {
        setError(SmNotInstantiated, tr("sensor has not been instantiated, no session to release"));
        return false;
    }

    bool returnValue = false;

    if(entryIt.value().sessions_.removeAll( sessionId ))
    {
        if ( entryIt.value().sessions_.empty() )
        {
            removeSensor(id);
        }
        returnValue = true;
    }
    else
    {
        // sessionId does not correspond to a request
        setError( SmNotInstantiated, tr("invalid sessionId, no session to release") );
    }

    socketHandler_->removeSession(sessionId);

    return returnValue;
}

AbstractChain* SensorManager::requestChain(const QString& id)
{
    clearError();

    AbstractChain* chain = NULL;
    QMap<QString, ChainInstanceEntry>::iterator entryIt = chainInstanceMap_.find(id);
    if (entryIt != chainInstanceMap_.end()) {
        if (entryIt.value().chain_ ) {
            chain = entryIt.value().chain_;
            entryIt.value().cnt_++;
        } else {
            QString type = entryIt.value().type_;
            if (chainFactoryMap_.contains(type)) {
                chain = chainFactoryMap_[type](id);
                Q_ASSERT(chain);

                entryIt.value().cnt_++;
                entryIt.value().chain_ = chain;
            } else {
                setError( SmFactoryNotRegistered, QString(tr("unknown chain type '%1'").arg(type)) );
            }
        }
    } else {
        setError( SmIdNotRegistered, QString(tr("unknown chain id '%1'").arg(id)) );
    }

    return chain;
}

void SensorManager::releaseChain(const QString& id)
{
    clearError();

    QMap<QString, ChainInstanceEntry>::iterator entryIt = chainInstanceMap_.find(id);
    if (entryIt != chainInstanceMap_.end()) {
        if (entryIt.value().chain_) {
            entryIt.value().cnt_--;

            if (entryIt.value().cnt_ == 0) {
                delete entryIt.value().chain_;
                entryIt.value().cnt_ = 0;
                entryIt.value().chain_ = 0;
            }
        } else {
            setError( SmNotInstantiated, QString(tr("chain '%1' not instantiated, cannot release").arg(id)) );
        }
    } else {
        setError( SmIdNotRegistered, QString(tr("unknown chain id '%1'").arg(id)) );
    }
}

DeviceAdaptor* SensorManager::requestDeviceAdaptor(const QString& id)
{
    Q_ASSERT( !id.contains(';') ); // no parameter passing support for adaptors

    clearError();

    DeviceAdaptor* da = NULL;
    QMap<QString, DeviceAdaptorInstanceEntry>::iterator entryIt = deviceAdaptorInstanceMap_.find(id);
    if ( entryIt != deviceAdaptorInstanceMap_.end() )
    {
        if ( entryIt.value().adaptor_ )
        {
            Q_ASSERT( entryIt.value().adaptor_ );
            //sensordLogD() << __PRETTY_FUNCTION__ << "instance exists already";
            da = entryIt.value().adaptor_;
            entryIt.value().cnt_++;
        }
        else
        {
            QString type = entryIt.value().type_;
            if ( deviceAdaptorFactoryMap_.contains(type) )
            {
                sensordLogD() << __PRETTY_FUNCTION__ << "new instance created:" << id;
                da = deviceAdaptorFactoryMap_[type](id);
                Q_ASSERT( da );

                ParameterParser::applyPropertyMap(da, entryIt.value().propertyMap_);

                bool ok = da->startAdaptor();
                if (ok)
                {
                    entryIt.value().adaptor_ = da;
                    entryIt.value().cnt_++;
                }
                else
                {
                    setError(SmAdaptorNotStarted, QString(tr("adaptor '%1' can not be started").arg(id)) );
                    delete da;
                    da = NULL;
                }
            }
            else
            {
                setError( SmFactoryNotRegistered, QString(tr("unknown adaptor type '%1'").arg(type)) );
            }
        }
    }
    else
    {
        setError( SmIdNotRegistered, QString(tr("unknown adaptor id '%1'").arg(id)) );
    }

    return da;
}

void SensorManager::releaseDeviceAdaptor(const QString& id)
{
    Q_ASSERT( !id.contains(';') ); // no parameter passing support for adaptors

    clearError();

    QMap<QString, DeviceAdaptorInstanceEntry>::iterator entryIt = deviceAdaptorInstanceMap_.find(id);
    if ( entryIt != deviceAdaptorInstanceMap_.end() )
    {
        if ( entryIt.value().adaptor_ )
        {
            Q_ASSERT( entryIt.value().adaptor_ );

            entryIt.value().cnt_--;
            if ( entryIt.value().cnt_ == 0 )
            {
                Q_ASSERT( entryIt.value().adaptor_ );

                entryIt.value().adaptor_->stopAdaptor();

                delete entryIt.value().adaptor_;
                entryIt.value().adaptor_ = 0;
                entryIt.value().cnt_ = 0;
            }
        }
        else
        {
            setError( SmNotInstantiated, QString(tr("adaptor '%1' not instantiated, cannot release").arg(id)) );
        }
    }
    else
    {
        setError( SmIdNotRegistered, QString(tr("unknown adaptor id '%1'").arg(id)) );
    }
}

FilterBase* SensorManager::instantiateFilter(const QString& id)
{
    QMap<QString, FilterFactoryMethod>::iterator it = filterFactoryMap_.find(id);
    if(it == filterFactoryMap_.end())
    {
        sensordLogW() << "Filter " << id << " not found.";
        return NULL;
    }
    return it.value()();
}

bool SensorManager::write(int id, const void* source, int size)
{
    if (!socketHandler_->write(id, source, size)) {
        sensordLogW() << "Failed to write data to socket.";
        return false;
    }
    return true;
}

void SensorManager::lostClient(int sessionId)
{
    for(QMap<QString, SensorInstanceEntry>::iterator it = sensorInstanceMap_.begin(); it != sensorInstanceMap_.end(); ++it) {
        if (it.value().sessions_.contains(sessionId)) {
            sensordLogD() << "[SensorManager]: Lost session " << sessionId << " detected as " << it.key();

            sensordLogD() << "[SensorManager]: Stopping sessionId " << sessionId;
            it.value().sensor_->stop(sessionId);

            sensordLogD() << "[SensorManager]: Releasing sessionId " << sessionId;
            releaseSensor(it.key(), sessionId);
            return;
        }
    }
    sensordLogW() << "[SensorManager]: Lost session " << sessionId << " detected, but not found from session list";
}

void SensorManager::displayStateChanged(const bool displayState)
{
    sensordLogD() << "Signal detected, display state changed to:" << displayState;

    displayState_ = displayState;

    if (displayState_) {
        /// Emit signal to make background calibration resume from sleep
        emit displayOn();
        if (!psmState_)
        {
            emit resumeCalibration();
        }
    }

    foreach (const DeviceAdaptorInstanceEntry& adaptor, deviceAdaptorInstanceMap_) {
        if (adaptor.adaptor_) {
            if (displayState) {
                adaptor.adaptor_->setScreenBlanked(false);
                adaptor.adaptor_->resume();

            } else {
                adaptor.adaptor_->setScreenBlanked(true);
                adaptor.adaptor_->standby();
            }
        }
    }
}


void SensorManager::devicePSMStateChanged(const bool psmState)
{
    psmState_ = psmState;
    if (psmState_)
    {
        emit stopCalibration();
    }
}


void SensorManager::printStatus(QStringList& output) const
{
    output.append("  Adaptors:\n");
    for (QMap<QString, DeviceAdaptorInstanceEntry>::const_iterator it = deviceAdaptorInstanceMap_.begin(); it != deviceAdaptorInstanceMap_.end(); ++it) {
        output.append(QString("    %1 [%2 listener(s)]\n").arg(it.value().type_).arg(it.value().cnt_));
    }

    output.append("  Chains:\n");
    for (QMap<QString, ChainInstanceEntry>::const_iterator it = chainInstanceMap_.begin(); it != chainInstanceMap_.end(); ++it) {
        output.append(QString("    %1 [%2 listener(s)]. %3\n").arg(it.value().type_).arg(it.value().cnt_).arg((it.value().chain_ && it.value().chain_->running()) ? "Running" : "Stopped"));
    }

    output.append("  Logical sensors:\n");
    for (QMap<QString, SensorInstanceEntry>::const_iterator it = sensorInstanceMap_.begin(); it != sensorInstanceMap_.end(); ++it) {

        QString str;
        str.append(QString("    %1 [").arg(it.value().type_));
        if(it.value().sessions_.size())
            str.append(QString("%1 session(s), PID(s): %2]").arg(it.value().sessions_.size()).arg(socketToPid(it.value().sessions_)));
        else
            str.append("No sessions]");
        str.append(QString(". %1\n").arg((it.value().sensor_ && it.value().sensor_->running()) ? "Running" : "Stopped"));
        output.append(str);
    }
}

QString SensorManager::socketToPid(int id) const
{
    struct ucred cr;
    socklen_t len = sizeof(cr);
    int fd = socketHandler_->getSocketFd(id);
    if (fd)
    {
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0)
            return QString("%1").arg(cr.pid);
        else
            return strerror(errno);
    }
    return "n/a";
}

QString SensorManager::socketToPid(QList<int> ids) const
{
    QString str;
    bool first = true;
    foreach (int id, ids)
    {
        if(!first)
            str.append(", ");
        first = false;
        str.append(socketToPid(id));
    }
    return str;
}

bool SensorManager::getPSMState()
{
    return psmState_;
}


#ifdef SM_PRINT
void SensorManager::print() const
{
    sensordLogD() << "Registry Dump:";
    foreach(const QString& id, sensorInstanceMap_.keys())
    {
        sensordLogD() << "Registry entry id  =" << id;

        sensordLogD() << "sessions     =" << sensorInstanceMap_[id].sessions_;
        sensordLogD() << "sensor             =" << sensorInstanceMap_[id].sensor_;
        sensordLogD() << "type               =" << sensorInstanceMap_[id].type_ << endl;
    }

    sensordLogD() << "sensorInstanceMap(" << sensorInstanceMap_.size() << "):" << sensorInstanceMap_.keys();
    sensordLogD() << "sensorFactoryMap(" << sensorFactoryMap_.size() << "):" << sensorFactoryMap_.keys();
}
#endif