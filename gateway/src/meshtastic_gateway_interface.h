#ifndef MESHTASTIC_GATEWAY_INTERFACE_H
#define MESHTASTIC_GATEWAY_INTERFACE_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include "interface.h"

class MeshtasticGatewayInterface : public PluginInterface
{
public:
    virtual ~MeshtasticGatewayInterface() = default;

    // JSON array of the connected node's channels:
    //   [{ index, name, displayName, role, psk, topic, shareUrl, relaying }]
    Q_INVOKABLE virtual QString getChannels() = 0;
    // Toggle whether a channel (by index) is bridged to its Logos Messaging topic.
    Q_INVOKABLE virtual void setRelay(int index, bool enabled) = 0;
    // JSON: { nodePresent, nodeName, channelCount }
    Q_INVOKABLE virtual QString status() = 0;

    // --- Chat ---
    // JSON array of a channel's messages, oldest first:
    //   [{ id, from, text, ts, outgoing, ackStatus, reactions:[{emoji,count}] }]
    //   outgoing = sent from this node; ackStatus in {queued,enroute,delivered,failed} (outgoing only)
    Q_INVOKABLE virtual QString getMessages(int channelIndex) = 0;
    // Send a message on a channel. Appends locally (outgoing) and, when the channel is
    // relaying, also publishes to its Logos Messaging topic. Emits "messagesChanged".
    Q_INVOKABLE virtual void sendMessage(int channelIndex, const QString& text) = 0;
    // Mark a channel's messages as read (zeroes its unread count). Emits "channelsChanged".
    Q_INVOKABLE virtual void markRead(int channelIndex) = 0;
    // Add an emoji reaction (tapback) to a message by id. Emits "messagesChanged".
    Q_INVOKABLE virtual void addReaction(int channelIndex, int messageId, const QString& emoji) = 0;
};

#define MeshtasticGatewayInterface_iid "org.logos.MeshtasticGatewayInterface"
Q_DECLARE_INTERFACE(MeshtasticGatewayInterface, MeshtasticGatewayInterface_iid)

#endif
