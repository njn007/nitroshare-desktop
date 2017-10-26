/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QSet>
#include <QVariantMap>

#include <nitroshare/application.h>
#include <nitroshare/logger.h>
#include <nitroshare/message.h>
#include <nitroshare/settingsregistry.h>

#include "broadcastenumerator.h"

const QString MessageTag = "broadcast";

const QString BroadcastInterval = "BroadcastInterval";
const QString BroadcastExpiry = "BroadcastExpiry";
const QString BroadcastPort = "BroadcastPort";

BroadcastEnumerator::BroadcastEnumerator(Application *application)
    : mApplication(application),
      mBroadcastInterval(Setting::Integer, BroadcastInterval, tr("Broadcast Interval"), 5000),
      mBroadcastExpiry(Setting::Integer, BroadcastExpiry, tr("Broadcast Expiry"), 30000),
      mBroadcastPort(Setting::Integer, BroadcastPort, tr("Broadcast Port"), 40816)

{
    connect(&mBroadcastTimer, &QTimer::timeout, this, &BroadcastEnumerator::onBroadcastTimeout);
    connect(&mExpiryTimer, &QTimer::timeout, this, &BroadcastEnumerator::onExpiryTimeout);
    connect(&mSocket, &QUdpSocket::readyRead, this, &BroadcastEnumerator::onReadyRead);
    connect(mApplication->settingsRegistry(), &SettingsRegistry::settingsChanged, this, &BroadcastEnumerator::onSettingsChanged);

    mApplication->settingsRegistry()->add(&mBroadcastInterval);
    mApplication->settingsRegistry()->add(&mBroadcastExpiry);
    mApplication->settingsRegistry()->add(&mBroadcastPort);

    // Trigger loading the initial settings
    onSettingsChanged({ BroadcastInterval, BroadcastExpiry, BroadcastPort });
}

BroadcastEnumerator::~BroadcastEnumerator()
{
    mApplication->settingsRegistry()->remove(&mBroadcastInterval);
    mApplication->settingsRegistry()->remove(&mBroadcastExpiry);
    mApplication->settingsRegistry()->remove(&mBroadcastPort);
}

void BroadcastEnumerator::onBroadcastTimeout()
{
    // Build a list of all IPv4 broadcast addresses
    QSet<QHostAddress> addresses;
    foreach (QNetworkInterface interface, QNetworkInterface::allInterfaces()) {
        if (interface.flags() & QNetworkInterface::CanBroadcast) {
            foreach (QNetworkAddressEntry entry, interface.addressEntries()) {
                if (!entry.broadcast().isNull()) {
                    addresses.insert(entry.broadcast());
                }
            }
        }
    }

    // Build the packet that will be broadcast
    QJsonObject object({
        { DeviceEnumerator::UuidKey, mApplication->deviceUuid() },
        { DeviceEnumerator::NameKey, mApplication->deviceName() }
    });
    QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);

    // Broadcast the packet
    foreach (QHostAddress address, addresses) {
        mSocket.writeDatagram(data, address, mSocket.localPort());
    }
}

void BroadcastEnumerator::onExpiryTimeout()
{
    // Grab the current time in MS and the timeout interval
    qint64 ms = QDateTime::currentMSecsSinceEpoch();
    int expiry = mApplication->settingsRegistry()->value(BroadcastExpiry).toInt();

    // Remove any devices that have expired
    auto i = mDevices.begin();
    while (i != mDevices.end()) {
        if (ms - i.value() > expiry) {
            mDevices.erase(i);
            emit deviceRemoved(i.key());
        }
        ++i;
    }
}

void BroadcastEnumerator::onReadyRead()
{
    while (mSocket.hasPendingDatagrams()) {

        // Capture both the data and the address
        QByteArray data;
        QHostAddress address;

        // Receive the packet
        data.resize(mSocket.pendingDatagramSize());
        mSocket.readDatagram(data.data(), data.size(), &address);

        // Extract the data from the packet
        QVariantMap properties = QJsonDocument::fromJson(data).object().toVariantMap();
        properties.insert("addresses", QStringList{ address.toString() });

        // Emit an update for the device
        QString uuid = properties.take("uuid").toString();
        emit deviceUpdated(uuid, properties);
    }
}

void BroadcastEnumerator::onSettingsChanged(const QStringList &keys)
{
    if (keys.contains(BroadcastInterval)) {
        mBroadcastTimer.stop();
        mBroadcastTimer.setInterval(
            mApplication->settingsRegistry()->value(BroadcastInterval).toInt()
        );
        onBroadcastTimeout();
        mBroadcastTimer.start();
    }

    if (keys.contains(BroadcastExpiry)) {
        mExpiryTimer.stop();
        mExpiryTimer.setInterval(
            mApplication->settingsRegistry()->value(BroadcastExpiry).toInt()
        );
        onExpiryTimeout();
        mExpiryTimer.start();
    }

    if (keys.contains(BroadcastPort)) {
        mSocket.close();
        if (!mSocket.bind(QHostAddress::Any,
                mApplication->settingsRegistry()->value(BroadcastPort).toInt())) {
            mApplication->logger()->log(new Message(
                Message::Error,
                MessageTag,
                mSocket.errorString()
            ));
        }
    }
}
