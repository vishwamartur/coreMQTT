/*
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <assert.h>

#include "mqtt_lightweight.h"
#include "private/mqtt_internal.h"

/**
 * @brief MQTT protocol version 3.1.1.
 */
#define MQTT_VERSION_3_1_1                          ( ( uint8_t ) 4U )

/**
 * @brief Size of the fixed and variable header of a CONNECT packet.
 */
#define MQTT_PACKET_CONNECT_HEADER_SIZE             ( 10UL )

/**
 * @brief Maximum size of an MQTT CONNECT packet, per MQTT spec.
 */
#define MQTT_PACKET_CONNECT_MAX_SIZE                ( 327700UL )

/* MQTT CONNECT flags. */
#define MQTT_CONNECT_FLAG_CLEAN                     ( 1 ) /**< @brief Clean session. */
#define MQTT_CONNECT_FLAG_WILL                      ( 2 ) /**< @brief Will present. */
#define MQTT_CONNECT_FLAG_WILL_QOS1                 ( 3 ) /**< @brief Will QoS 1. */
#define MQTT_CONNECT_FLAG_WILL_QOS2                 ( 4 ) /**< @brief Will QoS 2. */
#define MQTT_CONNECT_FLAG_WILL_RETAIN               ( 5 ) /**< @brief Will retain. */
#define MQTT_CONNECT_FLAG_PASSWORD                  ( 6 ) /**< @brief Password present. */
#define MQTT_CONNECT_FLAG_USERNAME                  ( 7 ) /**< @brief User name present. */

/*
 * Positions of each flag in the first byte of an MQTT PUBLISH packet's
 * fixed header.
 */
#define MQTT_PUBLISH_FLAG_RETAIN                    ( 0 ) /**< @brief MQTT PUBLISH retain flag. */
#define MQTT_PUBLISH_FLAG_QOS1                      ( 1 ) /**< @brief MQTT PUBLISH QoS1 flag. */
#define MQTT_PUBLISH_FLAG_QOS2                      ( 2 ) /**< @brief MQTT PUBLISH QoS2 flag. */
#define MQTT_PUBLISH_FLAG_DUP                       ( 3 ) /**< @brief MQTT PUBLISH duplicate flag. */

/**
 * @brief The size of MQTT DISCONNECT packets, per MQTT spec.
 */
#define MQTT_DISCONNECT_PACKET_SIZE                 ( 2UL )

/**
 * @brief The Remaining Length field of MQTT disconnect packets, per MQTT spec.
 */
#define MQTT_DISCONNECT_REMAINING_LENGTH            ( ( uint8_t ) 0 )

/*
 * Constants relating to CONNACK packets, defined by MQTT 3.1.1 spec.
 */
#define MQTT_PACKET_CONNACK_REMAINING_LENGTH        ( ( uint8_t ) 2U )    /**< @brief A CONNACK packet always has a "Remaining length" of 2. */
#define MQTT_PACKET_CONNACK_SESSION_PRESENT_MASK    ( ( uint8_t ) 0x01U ) /**< @brief The "Session Present" bit is always the lowest bit. */

/**
 * @brief The size of MQTT PUBACK, PUBREC, PUBREL, and PUBCOMP packets, per MQTT spec.
 */
#define MQTT_PUBLISH_ACK_PACKET_SIZE                ( 4U )

/*
 * UNSUBACK, PUBACK, PUBREC, PUBREL, and PUBCOMP always have a remaining length
 * of 2.
 */
#define MQTT_PACKET_SIMPLE_ACK_REMAINING_LENGTH     ( ( uint8_t ) 2 ) /**< @brief PUBACK, PUBREC, PUBREl, PUBCOMP, UNSUBACK Remaining length. */
#define MQTT_PACKET_PINGRESP_REMAINING_LENGTH       ( 0U )            /**< @brief A PINGRESP packet always has a "Remaining length" of 0. */

/**
 * @brief Per the MQTT 3.1.1 spec, the largest "Remaining Length" of an MQTT
 * packet is this value.
 */
#define MQTT_MAX_REMAINING_LENGTH                   ( 268435455UL )

/**
 * @brief Set a bit in an 8-bit unsigned integer.
 */
#define UINT8_SET_BIT( x, position )      ( ( x ) = ( uint8_t ) ( ( x ) | ( 0x01U << ( position ) ) ) )

/**
 * @brief Macro for checking if a bit is set in a 1-byte unsigned int.
 *
 * @param[in] x The unsigned int to check.
 * @param[in] position Which bit to check.
 */
#define UINT8_CHECK_BIT( x, position )    ( ( ( x ) & ( 0x01U << ( position ) ) ) == ( 0x01U << ( position ) ) )

/**
 * @brief Get the high byte of a 16-bit unsigned integer.
 */
#define UINT16_HIGH_BYTE( x )             ( ( uint8_t ) ( ( x ) >> 8 ) )

/**
 * @brief Get the low byte of a 16-bit unsigned integer.
 */
#define UINT16_LOW_BYTE( x )              ( ( uint8_t ) ( ( x ) & 0x00ffU ) )

/**
 * @brief Macro for decoding a 2-byte unsigned int from a sequence of bytes.
 *
 * @param[in] ptr A uint8_t* that points to the high byte.
 */
#define UINT16_DECODE( ptr )                                \
    ( uint16_t ) ( ( ( ( uint16_t ) ( *( ptr ) ) ) << 8 ) | \
                   ( ( uint16_t ) ( *( ( ptr ) + 1 ) ) ) )

/**
 * @brief A value that represents an invalid remaining length.
 *
 * This value is greater than what is allowed by the MQTT specification.
 */
#define MQTT_REMAINING_LENGTH_INVALID             ( ( size_t ) 268435456 )

/**
 * @brief The minimum remaining length for a QoS 0 PUBLISH.
 *
 * Includes two bytes for topic name length and one byte for topic name.
 */
#define MQTT_MIN_PUBLISH_REMAINING_LENGTH_QOS0    ( 3U )

/*-----------------------------------------------------------*/

/* MQTT Subscription packet types. */
typedef enum MQTTSubscriptionType
{
    MQTT_SUBSCRIBE,
    MQTT_UNSUBSCRIBE
} MQTTSubscriptionType_t;

/*-----------------------------------------------------------*/

/**
 * @brief Serializes MQTT PUBLISH packet into the buffer provided.
 *
 * This function serializes MQTT PUBLISH packet into #pFixedBuffer.pBuffer.
 * Copy of the payload into the buffer is done as part of the serialization
 * only if #serializePayload is true.
 *
 * @brief param[in] pPublishInfo Publish information.
 * @brief param[in] remainingLength Remaining length of the PUBLISH packet.
 * @brief param[in] packetIdentifier Packet identifier of PUBLISH packet.
 * @brief param[in, out] pFixedBuffer Buffer to which PUBLISH packet will be
 * serialized.
 * @brief param[in] serializePayload Copy payload to the serialized buffer
 * only if true. Only PUBLISH header will be serialized if false.
 *
 * @return Total number of bytes sent; -1 if there is an error.
 */
static void serializePublishCommon( const MQTTPublishInfo_t * pPublishInfo,
                                    size_t remainingLength,
                                    uint16_t packetIdentifier,
                                    const MQTTFixedBuffer_t * const pFixedBuffer,
                                    bool serializePayload );

/**
 * @brief Calculates the packet size and remaining length of an MQTT
 * PUBLISH packet.
 *
 * @param[in] pPublishInfo MQTT PUBLISH packet parameters.
 * @param[out] pRemainingLength The Remaining Length of the MQTT PUBLISH packet.
 * @param[out] pPacketSize The total size of the MQTT PUBLISH packet.
 *
 * @return false if the packet would exceed the size allowed by the
 * MQTT spec; true otherwise.
 */
static bool calculatePublishPacketSize( const MQTTPublishInfo_t * pPublishInfo,
                                        size_t * pRemainingLength,
                                        size_t * pPacketSize );

/**
 * @brief Calculates the packet size and remaining length of an MQTT
 * SUBSCRIBE or UNSUBSCRIBE packet.
 *
 * @param[in] pSubscriptionList List of MQTT subscription info.
 * @param[in] subscriptionCount The number of elements in pSubscriptionList.
 * @param[out] pRemainingLength The Remaining Length of the MQTT SUBSCRIBE or
 * UNSUBSCRIBE packet.
 * @param[out] pPacketSize The total size of the MQTT MQTT SUBSCRIBE or
 * UNSUBSCRIBE packet.
 * @param[in] subscriptionType #MQTT_SUBSCRIBE or #MQTT_UNSUBSCRIBE.
 *
 * #MQTTBadParameter if the packet would exceed the size allowed by the
 * MQTT spec; #MQTTSuccess otherwise.
 */
static MQTTStatus_t calculateSubscriptionPacketSize( const MQTTSubscribeInfo_t * pSubscriptionList,
                                                     size_t subscriptionCount,
                                                     size_t * pRemainingLength,
                                                     size_t * pPacketSize,
                                                     MQTTSubscriptionType_t subscriptionType );

/**
 * @brief Validates parameters of #MQTT_SerializeSubscribe or
 * #MQTT_SerializeUnsubscribe.
 *
 * @param[in] pSubscriptionList List of MQTT subscription info.
 * @param[in] subscriptionCount The number of elements in pSubscriptionList.
 * @param[in] packetId Packet identifier.
 * @param[in] remainingLength Remaining length of the packet.
 * @param[in] pBuffer Buffer for packet serialization.
 *
 * @return #MQTTNoMemory if pBuffer is too small to hold the MQTT packet;
 * #MQTTBadParameter if invalid parameters are passed;
 * #MQTTSuccess otherwise.
 */
static MQTTStatus_t validateSubscriptionSerializeParams( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                                         size_t subscriptionCount,
                                                         uint16_t packetId,
                                                         size_t remainingLength,
                                                         const MQTTFixedBuffer_t * const pBuffer );

/**
 * @brief Serialize an MQTT CONNECT packet in the given buffer.
 *
 * @param[in] pConnectInfo MQTT CONNECT packet parameters.
 * @param[in] pWillInfo Last Will and Testament. Pass NULL if not used.
 * @param[in] remainingLength Remaining Length of MQTT CONNECT packet.
 * @param[out] pBuffer Buffer for packet serialization.
 *
 */
static void serializeConnectPacket( const MQTTConnectInfo_t * const pConnectInfo,
                                    const MQTTPublishInfo_t * const pWillInfo,
                                    size_t remainingLength,
                                    const MQTTFixedBuffer_t * const pBuffer );

/*-----------------------------------------------------------*/

static size_t remainingLengthEncodedSize( size_t length )
{
    size_t encodedSize;

    /* Determine how many bytes are needed to encode length.
     * The values below are taken from the MQTT 3.1.1 spec. */

    /* 1 byte is needed to encode lengths between 0 and 127. */
    if( length < 128U )
    {
        encodedSize = 1U;
    }
    /* 2 bytes are needed to encode lengths between 128 and 16,383. */
    else if( length < 16384U )
    {
        encodedSize = 2U;
    }
    /* 3 bytes are needed to encode lengths between 16,384 and 2,097,151. */
    else if( length < 2097152U )
    {
        encodedSize = 3U;
    }
    /* 4 bytes are needed to encode lengths between 2,097,152 and 268,435,455. */
    else
    {
        encodedSize = 4U;
    }

    LogDebugWithArgs( "Encoded size for length =%ul is %ul.",
                      length,
                      encodedSize );

    return encodedSize;
}

/*-----------------------------------------------------------*/

static uint8_t * encodeRemainingLength( uint8_t * pDestination,
                                        size_t length )
{
    uint8_t lengthByte;
    uint8_t * pLengthEnd = NULL;
    size_t remainingLength = length;

    assert( pDestination != NULL );

    pLengthEnd = pDestination;

    /* This algorithm is copied from the MQTT v3.1.1 spec. */
    do
    {
        lengthByte = ( uint8_t ) ( remainingLength % 128U );
        remainingLength = remainingLength / 128U;

        /* Set the high bit of this byte, indicating that there's more data. */
        if( remainingLength > 0U )
        {
            UINT8_SET_BIT( lengthByte, 7 );
        }

        /* Output a single encoded byte. */
        *pLengthEnd = lengthByte;
        pLengthEnd++;
    } while( remainingLength > 0U );

    return pLengthEnd;
}

/*-----------------------------------------------------------*/

static uint8_t * encodeString( uint8_t * pDestination,
                               const char * source,
                               uint16_t sourceLength )
{
    uint8_t * pBuffer = NULL;

    /* Typecast const char * typed source buffer to const uint8_t *.
     * This is to use same type buffers in memcpy. */
    const uint8_t * pSourceBuffer = ( const uint8_t * ) source;

    assert( pDestination != NULL );

    pBuffer = pDestination;

    /* The first byte of a UTF-8 string is the high byte of the string length. */
    *pBuffer = UINT16_HIGH_BYTE( sourceLength );
    pBuffer++;

    /* The second byte of a UTF-8 string is the low byte of the string length. */
    *pBuffer = UINT16_LOW_BYTE( sourceLength );
    pBuffer++;

    /* Copy the string into pBuffer. */
    ( void ) memcpy( pBuffer, pSourceBuffer, sourceLength );

    /* Return the pointer to the end of the encoded string. */
    pBuffer += sourceLength;

    return pBuffer;
}

/*-----------------------------------------------------------*/

static bool calculatePublishPacketSize( const MQTTPublishInfo_t * pPublishInfo,
                                        size_t * pRemainingLength,
                                        size_t * pPacketSize )
{
    bool status = true;
    size_t packetSize = 0, payloadLimit = 0;

    assert( pPublishInfo != NULL );
    assert( pRemainingLength != NULL );
    assert( pPacketSize != NULL );

    /* The variable header of a PUBLISH packet always contains the topic name.
     * The first 2 bytes of UTF-8 string contains length of the string.
     */
    packetSize += pPublishInfo->topicNameLength + sizeof( uint16_t );

    /* The variable header of a QoS 1 or 2 PUBLISH packet contains a 2-byte
     * packet identifier. */
    if( pPublishInfo->qos > MQTTQoS0 )
    {
        packetSize += sizeof( uint16_t );
    }

    /* Calculate the maximum allowed size of the payload for the given parameters.
     * This calculation excludes the "Remaining length" encoding, whose size is not
     * yet known. */
    payloadLimit = MQTT_MAX_REMAINING_LENGTH - packetSize - 1U;

    /* Ensure that the given payload fits within the calculated limit. */
    if( pPublishInfo->payloadLength > payloadLimit )
    {
        LogErrorWithArgs( "PUBLISH payload length of %lu cannot exceed "
                          "%lu so as not to exceed the maximum "
                          "remaining length of MQTT 3.1.1 packet( %lu ).",
                          pPublishInfo->payloadLength,
                          payloadLimit,
                          MQTT_MAX_REMAINING_LENGTH );
        status = false;
    }
    else
    {
        /* Add the length of the PUBLISH payload. At this point, the "Remaining length"
         * has been calculated. */
        packetSize += pPublishInfo->payloadLength;

        /* Now that the "Remaining length" is known, recalculate the payload limit
         * based on the size of its encoding. */
        payloadLimit -= remainingLengthEncodedSize( packetSize );

        /* Check that the given payload fits within the size allowed by MQTT spec. */
        if( pPublishInfo->payloadLength > payloadLimit )
        {
            LogErrorWithArgs( "PUBLISH payload length of %lu cannot exceed "
                              "%lu so as not to exceed the maximum "
                              "remaining length of MQTT 3.1.1 packet( %lu ).",
                              pPublishInfo->payloadLength,
                              payloadLimit,
                              MQTT_MAX_REMAINING_LENGTH );
            status = false;
        }
        else
        {
            /* Set the "Remaining length" output parameter and calculate the full
             * size of the PUBLISH packet. */
            *pRemainingLength = packetSize;

            packetSize += 1U + remainingLengthEncodedSize( packetSize );
            *pPacketSize = packetSize;
        }
    }

    LogDebugWithArgs( "PUBLISH packet remaining length=%lu and packet size=%lu.",
                      *pRemainingLength,
                      *pPacketSize );
    return status;
}

/*-----------------------------------------------------------*/

static void serializePublishCommon( const MQTTPublishInfo_t * pPublishInfo,
                                    size_t remainingLength,
                                    uint16_t packetIdentifier,
                                    const MQTTFixedBuffer_t * const pFixedBuffer,
                                    bool serializePayload )
{
    uint8_t * pIndex = NULL;
    const uint8_t * pPayloadBuffer = NULL;

    /* The first byte of a PUBLISH packet contains the packet type and flags. */
    uint8_t publishFlags = MQTT_PACKET_TYPE_PUBLISH;

    assert( pPublishInfo != NULL );
    assert( pFixedBuffer != NULL );
    /* Packet Id should be non zero for QoS1 and QoS2. */
    assert( pPublishInfo->qos == MQTTQoS0 || packetIdentifier != 0U );
    /* Duplicate flag should be set only for Qos1 or Qos2. */
    assert( ( !pPublishInfo->dup ) || ( pPublishInfo->qos > MQTTQoS0 ) );

    /* Get the start address of the buffer. */
    pIndex = pFixedBuffer->pBuffer;

    if( pPublishInfo->qos == MQTTQoS1 )
    {
        LogDebug( "Adding QoS as QoS1 in PUBLISH flags." );
        UINT8_SET_BIT( publishFlags, MQTT_PUBLISH_FLAG_QOS1 );
    }
    else if( pPublishInfo->qos == MQTTQoS2 )
    {
        LogDebug( "Adding QoS as QoS2 in PUBLISH flags." );
        UINT8_SET_BIT( publishFlags, MQTT_PUBLISH_FLAG_QOS2 );
    }
    else
    {
        /* Empty else MISRA 15.7 */
    }

    if( pPublishInfo->retain )
    {
        LogDebug( "Adding retain bit in PUBLISH flags." );
        UINT8_SET_BIT( publishFlags, MQTT_PUBLISH_FLAG_RETAIN );
    }

    if( pPublishInfo->dup )
    {
        LogDebug( "Adding dup bit in PUBLISH flags." );
        UINT8_SET_BIT( publishFlags, MQTT_PUBLISH_FLAG_DUP );
    }

    *pIndex = publishFlags;
    pIndex++;

    /* The "Remaining length" is encoded from the second byte. */
    pIndex = encodeRemainingLength( pIndex, remainingLength );

    /* The topic name is placed after the "Remaining length". */
    pIndex = encodeString( pIndex,
                           pPublishInfo->pTopicName,
                           pPublishInfo->topicNameLength );

    /* A packet identifier is required for QoS 1 and 2 messages. */
    if( pPublishInfo->qos > MQTTQoS0 )
    {
        LogDebug( "Adding packet Id in PUBLISH packet." );
        /* Place the packet identifier into the PUBLISH packet. */
        *pIndex = UINT16_HIGH_BYTE( packetIdentifier );
        *( pIndex + 1 ) = UINT16_LOW_BYTE( packetIdentifier );
        pIndex += 2;
    }

    /* The payload is placed after the packet identifier.
     * Payload is copied over only if required by the flag serializePayload.
     * This will help reduce an unnecessary copy of the payload into the buffer.
     */
    if( ( pPublishInfo->payloadLength > 0U ) &&
        ( serializePayload ) )
    {
        LogDebugWithArgs( "Copying PUBLISH payload of length =%lu to buffer",
                          pPublishInfo->payloadLength );

        /* Typecast const void * typed payload buffer to const uint8_t *.
         * This is to use same type buffers in memcpy. */
        pPayloadBuffer = ( const uint8_t * ) pPublishInfo->pPayload;

        ( void ) memcpy( pIndex, pPayloadBuffer, pPublishInfo->payloadLength );
        pIndex += pPublishInfo->payloadLength;
    }

    /* Ensure that the difference between the end and beginning of the buffer
     * is less than the buffer size. */
    assert( ( ( size_t ) ( pIndex - pFixedBuffer->pBuffer ) ) <= pFixedBuffer->size );
}

static size_t getRemainingLength( MQTTTransportRecvFunc_t recvFunc,
                                  MQTTNetworkContext_t networkContext )
{
    size_t remainingLength = 0, multiplier = 1, bytesDecoded = 0, expectedSize = 0;
    uint8_t encodedByte = 0;
    int32_t bytesReceived = 0;

    /* This algorithm is copied from the MQTT v3.1.1 spec. */
    do
    {
        if( multiplier > 2097152U ) /* 128 ^ 3 */
        {
            remainingLength = MQTT_REMAINING_LENGTH_INVALID;
        }
        else
        {
            bytesReceived = recvFunc( networkContext, &encodedByte, 1U );

            if( bytesReceived == 1 )
            {
                remainingLength += ( ( size_t ) encodedByte & 0x7FU ) * multiplier;
                multiplier *= 128U;
                bytesDecoded++;
            }
            else
            {
                remainingLength = MQTT_REMAINING_LENGTH_INVALID;
            }
        }

        if( remainingLength == MQTT_REMAINING_LENGTH_INVALID )
        {
            break;
        }
    } while( ( encodedByte & 0x80U ) != 0U );

    /* Check that the decoded remaining length conforms to the MQTT specification. */
    if( remainingLength != MQTT_REMAINING_LENGTH_INVALID )
    {
        expectedSize = remainingLengthEncodedSize( remainingLength );

        if( bytesDecoded != expectedSize )
        {
            remainingLength = MQTT_REMAINING_LENGTH_INVALID;
        }
    }

    return remainingLength;
}

/*-----------------------------------------------------------*/

static bool incomingPacketValid( uint8_t packetType )
{
    bool status = false;

    /* Check packet type. Mask out lower bits to ignore flags. */
    switch( packetType & 0xF0U )
    {
        /* Valid incoming packet types. */
        case MQTT_PACKET_TYPE_CONNACK:
        case MQTT_PACKET_TYPE_PUBLISH:
        case MQTT_PACKET_TYPE_PUBACK:
        case MQTT_PACKET_TYPE_PUBREC:
        case MQTT_PACKET_TYPE_PUBCOMP:
        case MQTT_PACKET_TYPE_SUBACK:
        case MQTT_PACKET_TYPE_UNSUBACK:
        case MQTT_PACKET_TYPE_PINGRESP:
            status = true;
            break;

        case ( MQTT_PACKET_TYPE_PUBREL & 0xF0U ):

            /* The second bit of a PUBREL must be set. */
            if( ( packetType & 0x02U ) > 0U )
            {
                status = true;
            }

            break;

        /* Any other packet type is invalid. */
        default:
            LogWarnWithArgs( "Incoming packet invalid: Packet type=%u",
                             packetType );
            break;
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t checkPublishRemainingLength( size_t remainingLength,
                                                 MQTTQoS_t qos,
                                                 size_t qos0Minimum )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Sanity checks for "Remaining length". */
    if( qos == MQTTQoS0 )
    {
        /* Check that the "Remaining length" is greater than the minimum. */
        if( remainingLength < qos0Minimum )
        {
            LogDebugWithArgs( "QoS 0 PUBLISH cannot have a remaining length less than %lu.",
                              qos0Minimum );

            status = MQTTBadResponse;
        }
    }
    else
    {
        /* Check that the "Remaining length" is greater than the minimum. For
         * QoS 1 or 2, this will be two bytes greater than for QoS 0 due to the
         * packet identifier. */
        if( remainingLength < ( qos0Minimum + 2U ) )
        {
            LogDebugWithArgs( "QoS 1 or 2 PUBLISH cannot have a remaining length less than %lu.",
                              qos0Minimum + 2U );

            status = MQTTBadResponse;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t processPublishFlags( uint8_t publishFlags,
                                         MQTTPublishInfo_t * const pPublishInfo )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pPublishInfo != NULL );

    /* Check for QoS 2. */
    if( UINT8_CHECK_BIT( publishFlags, MQTT_PUBLISH_FLAG_QOS2 ) )
    {
        /* PUBLISH packet is invalid if both QoS 1 and QoS 2 bits are set. */
        if( UINT8_CHECK_BIT( publishFlags, MQTT_PUBLISH_FLAG_QOS1 ) )
        {
            LogDebug( "Bad QoS: 3." );

            status = MQTTBadResponse;
        }
        else
        {
            pPublishInfo->qos = MQTTQoS2;
        }
    }
    /* Check for QoS 1. */
    else if( UINT8_CHECK_BIT( publishFlags, MQTT_PUBLISH_FLAG_QOS1 ) )
    {
        pPublishInfo->qos = MQTTQoS1;
    }
    /* If the PUBLISH isn't QoS 1 or 2, then it's QoS 0. */
    else
    {
        pPublishInfo->qos = MQTTQoS0;
    }

    if( status == MQTTSuccess )
    {
        LogDebugWithArgs( "QoS is %d.", pPublishInfo->qos );

        /* Parse the Retain bit. */
        pPublishInfo->retain = UINT8_CHECK_BIT( publishFlags, MQTT_PUBLISH_FLAG_RETAIN );

        LogDebugWithArgs( "Retain bit is %d.", pPublishInfo->retain );

        /* Parse the DUP bit. */
        if( UINT8_CHECK_BIT( publishFlags, MQTT_PUBLISH_FLAG_DUP ) )
        {
            LogDebug( "DUP is 1." );
        }
        else
        {
            LogDebug( "DUP is 0." );
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static void logConnackResponse( uint8_t responseCode )
{
    /* Avoid unused parameter warning when assert and logs are disabled. */
    ( void ) responseCode;

    assert( responseCode <= 5 );

    /* Declare the CONNACK response code strings. The fourth byte of CONNACK
     * indexes into this array for the corresponding response. This array
     * does not need to be allocated if logs are not enabled. */
    #if LIBRARY_LOG_LEVEL > LOG_NONE
        static const char * const pConnackResponses[ 6 ] =
        {
            "Connection accepted.",                               /* 0 */
            "Connection refused: unacceptable protocol version.", /* 1 */
            "Connection refused: identifier rejected.",           /* 2 */
            "Connection refused: server unavailable",             /* 3 */
            "Connection refused: bad user name or password.",     /* 4 */
            "Connection refused: not authorized."                 /* 5 */
        };
        LogErrorWithArgs( "%s", pConnackResponses[ responseCode ] );
    #endif /* if LIBRARY_LOG_LEVEL > LOG_NONE */
}

/*-----------------------------------------------------------*/

static MQTTStatus_t deserializeConnack( const MQTTPacketInfo_t * const pConnack,
                                        bool * const pSessionPresent )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pConnack != NULL );
    assert( pSessionPresent != NULL );
    const uint8_t * pRemainingData = pConnack->pRemainingData;

    /* According to MQTT 3.1.1, the second byte of CONNACK must specify a
     * "Remaining length" of 2. */
    if( pConnack->remainingLength != MQTT_PACKET_CONNACK_REMAINING_LENGTH )
    {
        LogErrorWithArgs( "CONNACK does not have remaining length of %d.",
                          MQTT_PACKET_CONNACK_REMAINING_LENGTH );

        status = MQTTBadResponse;
    }

    /* Check the reserved bits in CONNACK. The high 7 bits of the second byte
     * in CONNACK must be 0. */
    else if( ( pRemainingData[ 0 ] | 0x01U ) != 0x01U )
    {
        LogError( "Reserved bits in CONNACK incorrect." );

        status = MQTTBadResponse;
    }
    else
    {
        /* Determine if the "Session Present" bit is set. This is the lowest bit of
         * the second byte in CONNACK. */
        if( ( pRemainingData[ 0 ] & MQTT_PACKET_CONNACK_SESSION_PRESENT_MASK )
            == MQTT_PACKET_CONNACK_SESSION_PRESENT_MASK )
        {
            LogWarn( "CONNACK session present bit set." );
            *pSessionPresent = true;

            /* MQTT 3.1.1 specifies that the fourth byte in CONNACK must be 0 if the
             * "Session Present" bit is set. */
            if( pRemainingData[ 1 ] != 0U )
            {
                status = MQTTBadResponse;
            }
        }
        else
        {
            LogInfo( "CONNACK session present bit not set." );
        }
    }

    if( status == MQTTSuccess )
    {
        /* In MQTT 3.1.1, only values 0 through 5 are valid CONNACK response codes. */
        if( pRemainingData[ 1 ] > 5U )
        {
            LogErrorWithArgs( "CONNACK response %hhu is not valid.",
                              pRemainingData[ 1 ] );

            status = MQTTBadResponse;
        }
        else
        {
            /* Print the appropriate message for the CONNACK response code if logs are
             * enabled. */
            logConnackResponse( pRemainingData[ 1 ] );

            /* A nonzero CONNACK response code means the connection was refused. */
            if( pRemainingData[ 1 ] > 0U )
            {
                status = MQTTServerRefused;
            }
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t calculateSubscriptionPacketSize( const MQTTSubscribeInfo_t * pSubscriptionList,
                                                     size_t subscriptionCount,
                                                     size_t * pRemainingLength,
                                                     size_t * pPacketSize,
                                                     MQTTSubscriptionType_t subscriptionType )
{
    MQTTStatus_t status = MQTTSuccess;
    size_t i = 0, packetSize = 0;

    assert( pSubscriptionList != NULL );
    assert( subscriptionCount != 0U );
    assert( pRemainingLength != NULL );
    assert( pPacketSize != NULL );

    /* The variable header of a subscription packet consists of a 2-byte packet
     * identifier. */
    packetSize += sizeof( uint16_t );

    /* Sum the lengths of all subscription topic filters; add 1 byte for each
     * subscription's QoS if type is IOT_MQTT_SUBSCRIBE. */
    for( i = 0; i < subscriptionCount; i++ )
    {
        /* Add the length of the topic filter. MQTT strings are prepended
         * with 2 byte string length field. Hence 2 bytes are added to size. */
        packetSize += pSubscriptionList[ i ].topicFilterLength + sizeof( uint16_t );

        /* Only SUBSCRIBE packets include the QoS. */
        if( subscriptionType == MQTT_SUBSCRIBE )
        {
            packetSize += 1U;
        }
    }

    /* At this point, the "Remaining length" has been calculated. Return error
     * if the "Remaining length" exceeds what is allowed by MQTT 3.1.1. Otherwise,
     * set the output parameter.*/
    if( packetSize > MQTT_MAX_REMAINING_LENGTH )
    {
        LogErrorWithArgs( "Subscription packet length of %lu exceeds"
                          "the MQTT 3.1.1 maximum packet length of %lu.",
                          packetSize,
                          MQTT_MAX_REMAINING_LENGTH );
        status = MQTTBadParameter;
    }
    else
    {
        *pRemainingLength = packetSize;

        /* Calculate the full size of the subscription packet by adding
         * number of bytes required to encode the "Remaining length" field
         * plus 1 byte for the "Packet type" field. */
        packetSize += 1U + remainingLengthEncodedSize( packetSize );

        /*Set the pPacketSize output parameter. */
        *pPacketSize = packetSize;
    }

    LogDebugWithArgs( "Subscription packet remaining length=%lu and packet size=%lu.",
                      *pRemainingLength,
                      *pPacketSize );

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t readSubackStatus( size_t statusCount,
                                      const uint8_t * pStatusStart )
{
    MQTTStatus_t status = MQTTSuccess;
    uint8_t subscriptionStatus = 0;
    size_t i = 0;

    assert( pStatusStart != NULL );

    /* Iterate through each status byte in the SUBACK packet. */
    for( i = 0; i < statusCount; i++ )
    {
        /* Read a single status byte in SUBACK. */
        subscriptionStatus = pStatusStart[ i ];

        /* MQTT 3.1.1 defines the following values as status codes. */
        switch( subscriptionStatus )
        {
            case 0x00:
            case 0x01:
            case 0x02:

                LogDebugWithArgs( "Topic filter %lu accepted, max QoS %hhu.",
                                  ( unsigned long ) i, subscriptionStatus );
                break;

            case 0x80:

                LogDebugWithArgs( "Topic filter %lu refused.", ( unsigned long ) i );

                /* Application should remove subscription from the list */
                status = MQTTServerRefused;

                break;

            default:
                LogDebugWithArgs( "Bad SUBSCRIBE status %hhu.", subscriptionStatus );

                status = MQTTBadResponse;

                break;
        }

        /* Stop parsing the subscription statuses if a bad response was received. */
        if( status == MQTTBadResponse )
        {
            break;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t deserializeSuback( const MQTTPacketInfo_t * const pSuback,
                                       uint16_t * pPacketIdentifier )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pSuback != NULL );
    assert( pPacketIdentifier != NULL );

    size_t remainingLength = pSuback->remainingLength;
    const uint8_t * pVariableHeader = pSuback->pRemainingData;

    /* A SUBACK must have a remaining length of at least 3 to accommodate the
     * packet identifier and at least 1 return code. */
    if( remainingLength < 3U )
    {
        LogDebug( "SUBACK cannot have a remaining length less than 3." );
        status = MQTTBadResponse;
    }
    else
    {
        /* Extract the packet identifier (first 2 bytes of variable header) from SUBACK. */
        *pPacketIdentifier = UINT16_DECODE( pVariableHeader );

        LogDebugWithArgs( "Packet identifier %hu.", *pPacketIdentifier );

        status = readSubackStatus( remainingLength - sizeof( uint16_t ),
                                   pVariableHeader + sizeof( uint16_t ) );
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t validateSubscriptionSerializeParams( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                                         size_t subscriptionCount,
                                                         uint16_t packetId,
                                                         size_t remainingLength,
                                                         const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    /* The serialized packet size = First byte
     *  + length of encoded size of remaining length
     *  + remaining length. */
    size_t packetSize = 1U + remainingLengthEncodedSize( remainingLength )
                        + remainingLength;

    /* Validate all the parameters. */
    if( ( pBuffer == NULL ) || ( pSubscriptionList == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pBuffer=%p, "
                          "pSubscriptionList=%p.",
                          pBuffer,
                          pSubscriptionList );
        status = MQTTBadParameter;
    }
    else if( subscriptionCount == 0U )
    {
        LogError( "Subscription count is 0." );
        status = MQTTBadParameter;
    }
    else if( packetId == 0U )
    {
        LogError( "Packet Id for subscription packet is 0." );
        status = MQTTBadParameter;
    }
    else if( packetSize > pBuffer->size )
    {
        LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                          "serialized packet of size of %lu.",
                          pBuffer->size,
                          packetSize );
        status = MQTTNoMemory;
    }
    else
    {
        /* Empty else MISRA 15.7 */
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t deserializePublish( const MQTTPacketInfo_t * const pIncomingPacket,
                                        uint16_t * const pPacketId,
                                        MQTTPublishInfo_t * const pPublishInfo )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pIncomingPacket != NULL );
    assert( pPacketId != NULL );
    assert( pPublishInfo != NULL );
    const uint8_t * pVariableHeader = pIncomingPacket->pRemainingData, * pPacketIdentifierHigh;
    /* The flags are the lower 4 bits of the first byte in PUBLISH. */
    status = processPublishFlags( ( pIncomingPacket->type & 0x0FU ), pPublishInfo );

    if( status == MQTTSuccess )
    {
        /* Sanity checks for "Remaining length". A QoS 0 PUBLISH  must have a remaining
         * length of at least 3 to accommodate topic name length (2 bytes) and topic
         * name (at least 1 byte). A QoS 1 or 2 PUBLISH must have a remaining length of
         * at least 5 for the packet identifier in addition to the topic name length and
         * topic name. */
        status = checkPublishRemainingLength( pIncomingPacket->remainingLength,
                                              pPublishInfo->qos,
                                              MQTT_MIN_PUBLISH_REMAINING_LENGTH_QOS0 );
    }

    if( status == MQTTSuccess )
    {
        /* Extract the topic name starting from the first byte of the variable header.
         * The topic name string starts at byte 3 in the variable header. */
        pPublishInfo->topicNameLength = UINT16_DECODE( pVariableHeader );

        /* Sanity checks for topic name length and "Remaining length". The remaining
         * length must be at least as large as the variable length header. */
        status = checkPublishRemainingLength( pIncomingPacket->remainingLength,
                                              pPublishInfo->qos,
                                              pPublishInfo->topicNameLength + sizeof( uint16_t ) );
    }

    if( status == MQTTSuccess )
    {
        /* Parse the topic. */
        pPublishInfo->pTopicName = ( const char * ) ( pVariableHeader + sizeof( uint16_t ) );
        LogDebugWithArgs( "Topic name length %hu: %.*s",
                          pPublishInfo->topicNameLength,
                          pPublishInfo->topicNameLength,
                          pPublishInfo->pTopicName );

        /* Extract the packet identifier for QoS 1 or 2 PUBLISH packets. Packet
         * identifier starts immediately after the topic name. */
        pPacketIdentifierHigh = ( const uint8_t * ) ( pPublishInfo->pTopicName + pPublishInfo->topicNameLength );

        if( pPublishInfo->qos > MQTTQoS0 )
        {
            *pPacketId = UINT16_DECODE( pPacketIdentifierHigh );

            LogDebugWithArgs( "Packet identifier %hu.", *pPacketId );

            /* Packet identifier cannot be 0. */
            if( *pPacketId == 0U )
            {
                status = MQTTBadResponse;
            }
        }
    }

    if( status == MQTTSuccess )
    {
        /* Calculate the length of the payload. QoS 1 or 2 PUBLISH packets contain
         * a packet identifier, but QoS 0 PUBLISH packets do not. */
        if( pPublishInfo->qos == MQTTQoS0 )
        {
            pPublishInfo->payloadLength = ( pIncomingPacket->remainingLength - pPublishInfo->topicNameLength - sizeof( uint16_t ) );
            pPublishInfo->pPayload = pPacketIdentifierHigh;
        }
        else
        {
            pPublishInfo->payloadLength = ( pIncomingPacket->remainingLength - pPublishInfo->topicNameLength - 2U * sizeof( uint16_t ) );
            pPublishInfo->pPayload = pPacketIdentifierHigh + sizeof( uint16_t );
        }

        LogDebugWithArgs( "Payload length %hu.", pPublishInfo->payloadLength );
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t deserializeSimpleAck( const MQTTPacketInfo_t * const pAck,
                                          uint16_t * pPacketIdentifier )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pAck != NULL );
    assert( pPacketIdentifier != NULL );

    /* Check that the "Remaining length" of the received ACK is 2. */
    if( pAck->remainingLength != MQTT_PACKET_SIMPLE_ACK_REMAINING_LENGTH )
    {
        LogErrorWithArgs( "ACK does not have remaining length of %d.",
                          MQTT_PACKET_SIMPLE_ACK_REMAINING_LENGTH );

        status = MQTTBadResponse;
    }
    else
    {
        /* Extract the packet identifier (third and fourth bytes) from ACK. */
        *pPacketIdentifier = UINT16_DECODE( pAck->pRemainingData );

        LogDebugWithArgs( "Packet identifier %hu.", *pPacketIdentifier );

        /* Packet identifier cannot be 0. */
        if( *pPacketIdentifier == 0U )
        {
            status = MQTTBadResponse;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static MQTTStatus_t deserializePingresp( const MQTTPacketInfo_t * const pPingresp )
{
    MQTTStatus_t status = MQTTSuccess;

    assert( pPingresp != NULL );

    /* Check the "Remaining length" (second byte) of the received PINGRESP is 0. */
    if( pPingresp->remainingLength != MQTT_PACKET_PINGRESP_REMAINING_LENGTH )
    {
        LogErrorWithArgs( "PINGRESP does not have remaining length of %d.",
                          MQTT_PACKET_PINGRESP_REMAINING_LENGTH );

        status = MQTTBadResponse;
    }

    return status;
}

/*-----------------------------------------------------------*/

static void serializeConnectPacket( const MQTTConnectInfo_t * const pConnectInfo,
                                    const MQTTPublishInfo_t * const pWillInfo,
                                    size_t remainingLength,
                                    const MQTTFixedBuffer_t * const pBuffer )
{
    uint8_t connectFlags = 0U;
    uint8_t * pIndex = NULL;

    assert( pConnectInfo != NULL );
    assert( pBuffer != NULL );

    pIndex = pBuffer->pBuffer;
    /* The first byte in the CONNECT packet is the control packet type. */
    *pIndex = MQTT_PACKET_TYPE_CONNECT;
    pIndex++;

    /* The remaining length of the CONNECT packet is encoded starting from the
     * second byte. The remaining length does not include the length of the fixed
     * header or the encoding of the remaining length. */
    pIndex = encodeRemainingLength( pIndex, remainingLength );

    /* The string "MQTT" is placed at the beginning of the CONNECT packet's variable
     * header. This string is 4 bytes long. */
    pIndex = encodeString( pIndex, "MQTT", 4 );

    /* The MQTT protocol version is the second field of the variable header. */
    *pIndex = MQTT_VERSION_3_1_1;
    pIndex++;

    /* Set the clean session flag if needed. */
    if( pConnectInfo->cleanSession == true )
    {
        UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_CLEAN );
    }

    /* Set the flags for username and password if provided. */
    if( pConnectInfo->pUserName != NULL )
    {
        UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_USERNAME );
    }

    if( pConnectInfo->pPassword != NULL )
    {
        UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_PASSWORD );
    }

    /* Set will flag if a Last Will and Testament is provided. */
    if( pWillInfo != NULL )
    {
        UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_WILL );

        /* Flags only need to be changed for Will QoS 1 or 2. */
        if( pWillInfo->qos == MQTTQoS1 )
        {
            UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_WILL_QOS1 );
        }
        else if( pWillInfo->qos == MQTTQoS2 )
        {
            UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_WILL_QOS2 );
        }
        else
        {
            /* Empty else MISRA 15.7 */
        }

        if( pWillInfo->retain == true )
        {
            UINT8_SET_BIT( connectFlags, MQTT_CONNECT_FLAG_WILL_RETAIN );
        }
    }

    *pIndex = connectFlags;
    pIndex++;

    /* Write the 2 bytes of the keep alive interval into the CONNECT packet. */
    *pIndex = UINT16_HIGH_BYTE( pConnectInfo->keepAliveSeconds );
    *( pIndex + 1 ) = UINT16_LOW_BYTE( pConnectInfo->keepAliveSeconds );
    pIndex += 2;

    /* Write the client identifier into the CONNECT packet. */
    pIndex = encodeString( pIndex,
                           pConnectInfo->pClientIdentifier,
                           pConnectInfo->clientIdentifierLength );

    /* Write the will topic name and message into the CONNECT packet if provided. */
    if( pWillInfo != NULL )
    {
        pIndex = encodeString( pIndex,
                               pWillInfo->pTopicName,
                               pWillInfo->topicNameLength );
        pIndex = encodeString( pIndex,
                               pWillInfo->pPayload,
                               ( uint16_t ) pWillInfo->payloadLength );
    }

    /* Encode the user name if provided. */
    if( pConnectInfo->pUserName != NULL )
    {
        pIndex = encodeString( pIndex, pConnectInfo->pUserName, pConnectInfo->userNameLength );
    }

    /* Encode the password if provided. */
    if( pConnectInfo->pPassword != NULL )
    {
        pIndex = encodeString( pIndex, pConnectInfo->pPassword, pConnectInfo->passwordLength );
    }

    LogDebugWithArgs( "Length of serialized CONNECT packet is %lu.",
                      ( ( size_t ) ( pIndex - pBuffer->pBuffer ) ) );

    /* Ensure that the difference between the end and beginning of the buffer
     * is less than the buffer size. */
    assert( ( ( size_t ) ( pIndex - pBuffer->pBuffer ) ) <= pBuffer->size );
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetConnectPacketSize( const MQTTConnectInfo_t * const pConnectInfo,
                                        const MQTTPublishInfo_t * const pWillInfo,
                                        size_t * const pRemainingLength,
                                        size_t * const pPacketSize )
{
    MQTTStatus_t status = MQTTSuccess;
    size_t remainingLength;

    /* The CONNECT packet will always include a 10-byte variable header. */
    size_t connectPacketSize = MQTT_PACKET_CONNECT_HEADER_SIZE;

    /* Validate arguments. */
    if( ( pConnectInfo == NULL ) || ( pRemainingLength == NULL ) ||
        ( pPacketSize == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pConnectInfo=%p, "
                          "pRemainingLength=%p, pPacketSize=%p.",
                          pConnectInfo,
                          pRemainingLength,
                          pPacketSize );
        status = MQTTBadParameter;
    }

    if( status == MQTTSuccess )
    {
        /* Add the length of the client identifier. */
        connectPacketSize += pConnectInfo->clientIdentifierLength + sizeof( uint16_t );

        /* Add the lengths of the will message and topic name if provided. */
        if( pWillInfo != NULL )
        {
            connectPacketSize += pWillInfo->topicNameLength + sizeof( uint16_t ) +
                                 pWillInfo->payloadLength + sizeof( uint16_t );
        }

        /* Add the lengths of the user name and password if provided. */
        if( pConnectInfo->pUserName != NULL )
        {
            connectPacketSize += pConnectInfo->userNameLength + sizeof( uint16_t );
        }

        if( pConnectInfo->pPassword != NULL )
        {
            connectPacketSize += pConnectInfo->passwordLength + sizeof( uint16_t );
        }

        /* At this point, the "Remaining Length" field of the MQTT CONNECT packet has
         * been calculated. */
        remainingLength = connectPacketSize;

        /* Calculate the full size of the MQTT CONNECT packet by adding the size of
         * the "Remaining Length" field plus 1 byte for the "Packet Type" field. */
        connectPacketSize += 1U + remainingLengthEncodedSize( connectPacketSize );

        /* Check that the CONNECT packet is within the bounds of the MQTT spec. */
        if( connectPacketSize > MQTT_PACKET_CONNECT_MAX_SIZE )
        {
            status = MQTTBadParameter;
        }
        else
        {
            *pRemainingLength = remainingLength;
            *pPacketSize = connectPacketSize;
        }

        LogDebugWithArgs( "CONNECT packet remaining length=%lu and packet size=%lu.",
                          *pRemainingLength,
                          *pPacketSize );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializeConnect( const MQTTConnectInfo_t * const pConnectInfo,
                                    const MQTTPublishInfo_t * const pWillInfo,
                                    size_t remainingLength,
                                    const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Calculate CONNECT packet size. */
    size_t connectPacketSize = remainingLength + remainingLengthEncodedSize( remainingLength ) + 1U;

    /* Validate arguments. */
    if( ( pConnectInfo == NULL ) || ( pBuffer == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pConnectInfo=%p, "
                          "pBuffer=%p.",
                          pConnectInfo,
                          pBuffer );
        status = MQTTBadParameter;
    }
    /* Check that the full packet size fits within the given buffer. */
    else if( connectPacketSize > pBuffer->size )
    {
        LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                          "serialized CONNECT packet of size of %lu.",
                          pBuffer->size,
                          connectPacketSize );
        status = MQTTNoMemory;
    }
    else
    {
        serializeConnectPacket( pConnectInfo,
                                pWillInfo,
                                remainingLength,
                                pBuffer );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetSubscribePacketSize( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                          size_t subscriptionCount,
                                          size_t * pRemainingLength,
                                          size_t * pPacketSize )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Validate parameters. */
    if( ( pSubscriptionList == NULL ) || ( pRemainingLength == NULL ) ||
        ( pPacketSize == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pSubscriptionList=%p, "
                          "pRemainingLength=%p, pPacketSize=%p.",
                          pSubscriptionList,
                          pRemainingLength,
                          pPacketSize );
        status = MQTTBadParameter;
    }
    else if( subscriptionCount == 0U )
    {
        LogError( " subscriptionCount is 0." );
        status = MQTTBadParameter;
    }
    else
    {
        /* Calculate the MQTT UNSUBSCRIBE packet size. */
        status = calculateSubscriptionPacketSize( pSubscriptionList,
                                                  subscriptionCount,
                                                  pRemainingLength,
                                                  pPacketSize,
                                                  MQTT_SUBSCRIBE );

        if( status == MQTTBadParameter )
        {
            LogErrorWithArgs( "SUBSCRIBE packet remaining length exceeds %lu, which is the "
                              "maximum size allowed by MQTT 3.1.1.",
                              MQTT_MAX_REMAINING_LENGTH );
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializeSubscribe( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                      size_t subscriptionCount,
                                      uint16_t packetId,
                                      size_t remainingLength,
                                      const MQTTFixedBuffer_t * const pBuffer )
{
    size_t i = 0;
    uint8_t * pIndex = NULL;

    /* Validate all the parameters. */
    MQTTStatus_t status =
        validateSubscriptionSerializeParams( pSubscriptionList,
                                             subscriptionCount,
                                             packetId,
                                             remainingLength,
                                             pBuffer );

    if( status == MQTTSuccess )
    {
        pIndex = pBuffer->pBuffer;

        /* The first byte in SUBSCRIBE is the packet type. */
        *pIndex = MQTT_PACKET_TYPE_SUBSCRIBE;
        pIndex++;

        /* Encode the "Remaining length" starting from the second byte. */
        pIndex = encodeRemainingLength( pIndex, remainingLength );

        /* Place the packet identifier into the SUBSCRIBE packet. */
        *pIndex = UINT16_HIGH_BYTE( packetId );
        *( pIndex + 1 ) = UINT16_LOW_BYTE( packetId );
        pIndex += 2;

        /* Serialize each subscription topic filter and QoS. */
        for( i = 0; i < subscriptionCount; i++ )
        {
            pIndex = encodeString( pIndex,
                                   pSubscriptionList[ i ].pTopicFilter,
                                   pSubscriptionList[ i ].topicFilterLength );

            /* Place the QoS in the SUBSCRIBE packet. */
            *pIndex = ( uint8_t ) ( pSubscriptionList[ i ].qos );
            pIndex++;
        }

        LogDebugWithArgs( "Length of serialized SUBSCRIBE packet is %lu.",
                          ( ( size_t ) ( pIndex - pBuffer->pBuffer ) ) );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetUnsubscribePacketSize( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                            size_t subscriptionCount,
                                            size_t * pRemainingLength,
                                            size_t * pPacketSize )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Validate parameters. */
    if( ( pSubscriptionList == NULL ) || ( pRemainingLength == NULL ) ||
        ( pPacketSize == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pSubscriptionList=%p, "
                          "pRemainingLength=%p, pPacketSize=%p.",
                          pSubscriptionList,
                          pRemainingLength,
                          pPacketSize );
        status = MQTTBadParameter;
    }
    else if( subscriptionCount == 0U )
    {
        LogError( "Subscription count is 0." );
        status = MQTTBadParameter;
    }
    else
    {
        /* Calculate the MQTT SUBSCRIBE packet size. */
        status = calculateSubscriptionPacketSize( pSubscriptionList,
                                                  subscriptionCount,
                                                  pRemainingLength,
                                                  pPacketSize,
                                                  MQTT_SUBSCRIBE );

        if( status == MQTTBadParameter )
        {
            LogErrorWithArgs( "UNSUBSCRIBE packet remaining length exceeds %lu, which is the "
                              "maximum size allowed by MQTT 3.1.1.",
                              MQTT_MAX_REMAINING_LENGTH );
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializeUnsubscribe( const MQTTSubscribeInfo_t * const pSubscriptionList,
                                        size_t subscriptionCount,
                                        uint16_t packetId,
                                        size_t remainingLength,
                                        const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    size_t i = 0;
    uint8_t * pIndex = NULL;

    /* Validate all the parameters. */
    status = validateSubscriptionSerializeParams( pSubscriptionList,
                                                  subscriptionCount,
                                                  packetId,
                                                  remainingLength,
                                                  pBuffer );

    if( status == MQTTSuccess )
    {
        /* Get the start of the buffer to the iterator variable. */
        pIndex = pBuffer->pBuffer;

        /* The first byte in UNSUBSCRIBE is the packet type. */
        *pIndex = MQTT_PACKET_TYPE_UNSUBSCRIBE;
        pIndex++;

        /* Encode the "Remaining length" starting from the second byte. */
        pIndex = encodeRemainingLength( pIndex, remainingLength );

        /* Place the packet identifier into the UNSUBSCRIBE packet. */
        *pIndex = UINT16_HIGH_BYTE( packetId );
        *( pIndex + 1 ) = UINT16_LOW_BYTE( packetId );
        pIndex += 2;

        /* Serialize each subscription topic filter. */
        for( i = 0; i < subscriptionCount; i++ )
        {
            pIndex = encodeString( pIndex,
                                   pSubscriptionList[ i ].pTopicFilter,
                                   pSubscriptionList[ i ].topicFilterLength );
        }

        LogDebugWithArgs( "Length of serialized UNSUBSCRIBE packet is %lu.",
                          ( ( size_t ) ( pIndex - pBuffer->pBuffer ) ) );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetPublishPacketSize( const MQTTPublishInfo_t * const pPublishInfo,
                                        size_t * const pRemainingLength,
                                        size_t * const pPacketSize )
{
    MQTTStatus_t status = MQTTSuccess;

    if( ( pPublishInfo == NULL ) || ( pRemainingLength == NULL ) || ( pPacketSize == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pPublishInfo=%p, "
                          "pRemainingLength=%p, pPacketSize=%p.",
                          pPublishInfo,
                          pRemainingLength,
                          pPacketSize );
        status = MQTTBadParameter;
    }
    else if( ( pPublishInfo->pTopicName == NULL ) || ( pPublishInfo->topicNameLength == 0U ) )
    {
        LogErrorWithArgs( "Invalid topic name for PUBLISH: pTopicName=%p, "
                          "topicNameLength=%u.",
                          pPublishInfo->pTopicName,
                          pPublishInfo->topicNameLength );
        status = MQTTBadParameter;
    }
    else
    {
        /* Calculate the "Remaining length" field and total packet size. If it exceeds
         * what is allowed in the MQTT standard, return an error. */
        if( calculatePublishPacketSize( pPublishInfo, pRemainingLength, pPacketSize ) == false )
        {
            LogErrorWithArgs( "PUBLISH packet remaining length exceeds %lu, which is the "
                              "maximum size allowed by MQTT 3.1.1.",
                              MQTT_MAX_REMAINING_LENGTH );
            status = MQTTBadParameter;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializePublish( const MQTTPublishInfo_t * const pPublishInfo,
                                    uint16_t packetId,
                                    size_t remainingLength,
                                    const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Length of serialized packet = First byte
     *                               + Length of encoded remaining length
     *                               + Remaining length. */
    size_t packetSize = 1U + remainingLengthEncodedSize( remainingLength )
                        + remainingLength;

    if( ( pBuffer == NULL ) || ( pPublishInfo == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pBuffer=%p, "
                          "pPublishInfo=%p.",
                          pBuffer,
                          pPublishInfo );
        status = MQTTBadParameter;
    }
    else if( ( pPublishInfo->pTopicName == NULL ) || ( pPublishInfo->topicNameLength == 0U ) )
    {
        LogErrorWithArgs( "Invalid topic name for PUBLISH: pTopicName=%p, "
                          "topicNameLength=%u.",
                          pPublishInfo->pTopicName,
                          pPublishInfo->topicNameLength );
        status = MQTTBadParameter;
    }
    else if( ( pPublishInfo->qos != MQTTQoS0 ) && ( packetId == 0U ) )
    {
        LogErrorWithArgs( "Packet Id is 0 for PUBLISH with QoS=%u.",
                          pPublishInfo->qos );
        status = MQTTBadParameter;
    }
    else if( packetSize > pBuffer->size )
    {
        LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                          "serialized PUBLISH packet of size of %lu.",
                          pBuffer->size,
                          packetSize );
        status = MQTTNoMemory;
    }
    else
    {
        /* Serialize publish with header and payload. */
        serializePublishCommon( pPublishInfo,
                                remainingLength,
                                packetId,
                                pBuffer,
                                true );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializePublishHeader( const MQTTPublishInfo_t * const pPublishInfo,
                                          uint16_t packetId,
                                          size_t remainingLength,
                                          const MQTTFixedBuffer_t * const pBuffer,
                                          size_t * const pHeaderSize )
{
    MQTTStatus_t status = MQTTSuccess;

    /* Length of serialized packet = First byte
     *                               + Length of encoded remaining length
     *                               + Remaining length
     *                               - Payload Length.
     * Payload length will be subtracted after verifying pPublishInfo parameter.
     */
    size_t packetSize = 1U + remainingLengthEncodedSize( remainingLength )
                        + remainingLength;

    if( ( pBuffer == NULL ) || ( pPublishInfo == NULL ) ||
        ( pHeaderSize == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pBuffer=%p, "
                          "pPublishInfo=%p, pHeaderSize=%p.",
                          pBuffer,
                          pPublishInfo,
                          pHeaderSize );
        status = MQTTBadParameter;
    }
    else if( ( pPublishInfo->pTopicName == NULL ) || ( pPublishInfo->topicNameLength == 0U ) )
    {
        LogErrorWithArgs( "Invalid topic name for publish: pTopicName=%p, "
                          "topicNameLength=%u.",
                          pPublishInfo->pTopicName,
                          pPublishInfo->topicNameLength );
        status = MQTTBadParameter;
    }
    else if( ( pPublishInfo->qos != MQTTQoS0 ) && ( packetId == 0U ) )
    {
        LogErrorWithArgs( "Packet Id is 0 for publish with QoS=%u.",
                          pPublishInfo->qos );
        status = MQTTBadParameter;
    }


    else if( ( packetSize - pPublishInfo->payloadLength ) > pBuffer->size )
    {
        LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                          "serialized PUBLISH header packet of size of %lu.",
                          pBuffer->size,
                          ( packetSize - pPublishInfo->payloadLength ) );
        status = MQTTNoMemory;
    }
    else
    {
        /* Serialize publish without copying the payload. */
        serializePublishCommon( pPublishInfo,
                                remainingLength,
                                packetId,
                                pBuffer,
                                false );

        /* Header size is the same as calculated packet size. */
        *pHeaderSize = ( packetSize - pPublishInfo->payloadLength );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializeAck( const MQTTFixedBuffer_t * const pBuffer,
                                uint8_t packetType,
                                uint16_t packetId )
{
    MQTTStatus_t status = MQTTSuccess;

    if( pBuffer == NULL )
    {
        LogError( "Provided buffer is NULL." );
        status = MQTTBadParameter;
    }
    /* The buffer must be able to fit 4 bytes for the packet. */
    else if( pBuffer->size < MQTT_PUBLISH_ACK_PACKET_SIZE )
    {
        LogError( "Insufficient memory for packet." );
        status = MQTTNoMemory;
    }
    else
    {
        switch( packetType )
        {
            /* Only publish acks are serialized by the client. */
            case MQTT_PACKET_TYPE_PUBACK:
            case MQTT_PACKET_TYPE_PUBREC:
            case MQTT_PACKET_TYPE_PUBREL:
            case MQTT_PACKET_TYPE_PUBCOMP:
                pBuffer->pBuffer[ 0 ] = packetType;
                pBuffer->pBuffer[ 1 ] = MQTT_PACKET_SIMPLE_ACK_REMAINING_LENGTH;
                pBuffer->pBuffer[ 2 ] = UINT16_HIGH_BYTE( packetId );
                pBuffer->pBuffer[ 3 ] = UINT16_LOW_BYTE( packetId );
                break;

            default:
                LogErrorWithArgs( "Packet type is not a publish ACK: Packet type=%02x",
                                  packetType );
                status = MQTTBadParameter;
                break;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetDisconnectPacketSize( size_t * pPacketSize )
{
    /* MQTT DISCONNECT packets always have the same size. */
    *pPacketSize = MQTT_DISCONNECT_PACKET_SIZE;

    return MQTTSuccess;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializeDisconnect( const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;
    size_t disconnectPacketSize;

    /* Validate arguments. */
    if( pBuffer == NULL )
    {
        LogError( "pBuffer cannot be NULL." );
        status = MQTTBadParameter;
    }

    if( status == MQTTSuccess )
    {
        status = MQTT_GetDisconnectPacketSize( &disconnectPacketSize );
        LogDebugWithArgs( "MQTT DISCONNECT packet size is %ul.",
                          disconnectPacketSize );
    }

    if( status == MQTTSuccess )
    {
        if( pBuffer->size < disconnectPacketSize )
        {
            LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                              "serialized DISCONNECT packet of size of %lu.",
                              pBuffer->size,
                              disconnectPacketSize );
            status = MQTTNoMemory;
        }
    }

    if( status == MQTTSuccess )
    {
        pBuffer->pBuffer[ 0 ] = MQTT_PACKET_TYPE_DISCONNECT;
        pBuffer->pBuffer[ 1 ] = MQTT_DISCONNECT_REMAINING_LENGTH;
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_SerializePingreq( const MQTTFixedBuffer_t * const pBuffer )
{
    MQTTStatus_t status = MQTTSuccess;

    if( pBuffer == NULL )
    {
        LogError( "pBuffer is NULL." );
        status = MQTTBadParameter;
    }
    else if( pBuffer->size < MQTT_PACKET_PINGREQ_SIZE )
    {
        LogErrorWithArgs( "Buffer size of %lu is not sufficient to hold "
                          "serialized PINGREQ packet of size of %lu.",
                          pBuffer->size,
                          MQTT_PACKET_PINGREQ_SIZE );
        status = MQTTNoMemory;
    }
    else
    {
        /* Ping request packets are always the same. */
        pBuffer->pBuffer[ 0 ] = MQTT_PACKET_TYPE_PINGREQ;
        pBuffer->pBuffer[ 1 ] = 0x00;
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetIncomingPacket( MQTTTransportRecvFunc_t recvFunc,
                                     MQTTNetworkContext_t networkContext,
                                     MQTTPacketInfo_t * const pIncomingPacket )
{
    return MQTTSuccess;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_DeserializePublish( const MQTTPacketInfo_t * const pIncomingPacket,
                                      uint16_t * const pPacketId,
                                      MQTTPublishInfo_t * const pPublishInfo )
{
    MQTTStatus_t status = MQTTSuccess;

    if( ( pIncomingPacket == NULL ) || ( pPacketId == NULL ) || ( pPublishInfo == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pIncomingPacket=%p, "
                          "pPacketId=%p, pPublishInfo=%p",
                          pIncomingPacket,
                          pPacketId,
                          pPublishInfo );
        status = MQTTBadParameter;
    }
    else if( ( pIncomingPacket->type & 0xF0U ) != MQTT_PACKET_TYPE_PUBLISH )
    {
        LogErrorWithArgs( "Packet is not publish. Packet type: %hu.",
                          pIncomingPacket->type );
        status = MQTTBadParameter;
    }
    else
    {
        status = deserializePublish( pIncomingPacket, pPacketId, pPublishInfo );
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_DeserializeAck( const MQTTPacketInfo_t * const pIncomingPacket,
                                  uint16_t * const pPacketId,
                                  bool * const pSessionPresent )
{
    MQTTStatus_t status = MQTTSuccess;

    if( ( pIncomingPacket == NULL ) || ( pPacketId == NULL ) || ( pSessionPresent == NULL ) )
    {
        LogErrorWithArgs( "Argument cannot be NULL: pIncomingPacket=%p, "
                          "pPacketId=%p, pSessionPresent=%p",
                          pIncomingPacket,
                          pPacketId,
                          pSessionPresent );
        status = MQTTBadParameter;
    }
    else if( pIncomingPacket->pRemainingData == NULL )
    {
        LogError( "Remaining data of incoming packet is NULL." );
        status = MQTTBadParameter;
    }
    else
    {
        /* Make sure response packet is a valid ack. */
        switch( pIncomingPacket->type )
        {
            case MQTT_PACKET_TYPE_CONNACK:
                status = deserializeConnack( pIncomingPacket, pSessionPresent );
                break;

            case MQTT_PACKET_TYPE_SUBACK:
                status = deserializeSuback( pIncomingPacket, pPacketId );
                break;

            case MQTT_PACKET_TYPE_PINGRESP:
                status = deserializePingresp( pIncomingPacket );
                break;

            case MQTT_PACKET_TYPE_UNSUBACK:
            case MQTT_PACKET_TYPE_PUBACK:
            case MQTT_PACKET_TYPE_PUBREC:
            case MQTT_PACKET_TYPE_PUBREL:
            case MQTT_PACKET_TYPE_PUBCOMP:
                status = deserializeSimpleAck( pIncomingPacket, pPacketId );
                break;

            /* Any other packet type is invalid. */
            default:
                LogErrorWithArgs( "IotMqtt_DeserializeResponse() called with unknown packet type:(%lu).", pIncomingPacket->type );
                status = MQTTBadResponse;
                break;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

MQTTStatus_t MQTT_GetIncomingPacketTypeAndLength( MQTTTransportRecvFunc_t readFunc,
                                                  MQTTNetworkContext_t networkContext,
                                                  MQTTPacketInfo_t * pIncomingPacket )
{
    MQTTStatus_t status = MQTTSuccess;
    /* Read a single byte. */
    int32_t bytesReceived = readFunc( networkContext, &( pIncomingPacket->type ), 1U );

    if( bytesReceived == 1 )
    {
        /* Check validity. */
        if( incomingPacketValid( pIncomingPacket->type ) )
        {
            pIncomingPacket->remainingLength = getRemainingLength( readFunc, networkContext );

            if( pIncomingPacket->remainingLength == MQTT_REMAINING_LENGTH_INVALID )
            {
                status = MQTTBadResponse;
            }
        }
        else
        {
            LogErrorWithArgs( "Incoming packet invalid: Packet type=%u",
                              pIncomingPacket->type );
            status = MQTTBadResponse;
        }
    }
    else
    {
        status = MQTTNoDataAvailable;
    }

    return status;
}

/*-----------------------------------------------------------*/