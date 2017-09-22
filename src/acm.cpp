/** 
 * @copyright Copyright 2017 US DOT - Joint Program Office
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *    Oak Ridge National Laboratory, Center for Trustworthy Embedded Systems, UT Battelle.
 *
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2014, Magnus Edenhill
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "acm.hpp"
#include "utilities.hpp"

#include "spdlog/spdlog.h"


#include <csignal>
#include <chrono>
#include <thread>
#include <cstdio>

// for both windows and linux.
#include <sys/types.h>
#include <sys/stat.h>

#ifndef _MSC_VER
#include <sys/time.h>
#endif

#ifdef _MSC_VER
#include "../win32/wingetopt.h"
#include <atltime.h>
#elif _AIX
#include <unistd.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif

/**
 * @brief predicate indicating whether a file exists on the filesystem.
 *
 * @return true if it exists, false otherwise.
 */
bool fileExists( const std::string& s ) {
    struct stat info;

    if (stat( s.c_str(), &info) != 0) {     // bad stat; doesn't exist.
        return false;
    } else if (info.st_mode & S_IFREG) {    // exists and is a regular file.
        return true;
    } 

    return false;
}

/**
 * @brief predicate indicating whether a path/directory exists on the filesystem.
 *
 * @return true if it exists, false otherwise.
 */
bool dirExists( const std::string& s ) {
    struct stat info;

    if (stat( s.c_str(), &info) != 0) {     // bad stat; doesn't exist.
        return false;
    } else if (info.st_mode & S_IFDIR) {    // exists and is a directory.
        return true;
    } 

    return false;
}

bool ASN1_Codec::data_available = true;

void ASN1_Codec::sigterm (int sig) {
    data_available = false;
}

ASN1_Codec::ASN1_Codec( const std::string& name, const std::string& description ) :
    Tool{ name, description },
    exit_eof{true},
    eof_cnt{0},
    partition_cnt{1},
    msg_recv_count{0},
    msg_send_count{0},
    msg_filt_count{0},
    msg_recv_bytes{0},
    msg_send_bytes{0},
    msg_filt_bytes{0},
    iloglevel{ spdlog::level::trace },
    eloglevel{ spdlog::level::err },
    pconf{},
    brokers{"localhost"},
    partition{RdKafka::Topic::PARTITION_UA},
    mode{""},
    debug{""},
    consumed_topics{},
    offset{RdKafka::Topic::OFFSET_BEGINNING},
    published_topic_name{},
    conf{nullptr},
    tconf{nullptr},
    consumer_ptr{},
    consumer_timeout{500},
    producer_ptr{},
    published_topic_ptr{},
    ilogger{},
    elogger{},
    first_block{ true }
{
    // dump_file = fopen( "dump.file.dat", "wb" );
}

ASN1_Codec::~ASN1_Codec() 
{
    // fclose(dump_file);

    if (consumer_ptr) {
        consumer_ptr->close();
    }

    // free raw librdkafka pointers.
    if (tconf) delete tconf;
    if (conf) delete conf;

    // TODO: This librdkafka item seems wrong...
    RdKafka::wait_destroyed(5000);    // pause to let RdKafka reclaim resources.
}

void ASN1_Codec::metadata_print (const std::string &topic, const RdKafka::Metadata *metadata) {

    std::cout << "Metadata for " << (topic.empty() ? "" : "all topics")
        << "(from broker "  << metadata->orig_broker_id()
        << ":" << metadata->orig_broker_name() << std::endl;

    /* Iterate brokers */
    std::cout << " " << metadata->brokers()->size() << " brokers:" << std::endl;

    for ( auto ib : *(metadata->brokers()) ) {
        std::cout << "  broker " << ib->id() << " at " << ib->host() << ":" << ib->port() << std::endl;
    }

    /* Iterate topics */
    std::cout << metadata->topics()->size() << " topics:" << std::endl;

    for ( auto& it : *(metadata->topics()) ) {

        std::cout << "  topic \""<< it->topic() << "\" with " << it->partitions()->size() << " partitions:";

        if (it->err() != RdKafka::ERR_NO_ERROR) {
            std::cout << " " << err2str(it->err());
            if (it->err() == RdKafka::ERR_LEADER_NOT_AVAILABLE) std::cout << " (try again)";
        }

        std::cout << std::endl;

        /* Iterate topic's partitions */
        for ( auto& ip : *(it->partitions()) ) {
            std::cout << "    partition " << ip->id() << ", leader " << ip->leader() << ", replicas: ";

            /* Iterate partition's replicas */
            RdKafka::PartitionMetadata::ReplicasIterator ir;
            for (ir = ip->replicas()->begin(); ir != ip->replicas()->end(); ++ir) {
                std::cout << (ir == ip->replicas()->begin() ? "":",") << *ir;
            }

            /* Iterate partition's ISRs */
            std::cout << ", isrs: ";
            RdKafka::PartitionMetadata::ISRSIterator iis;
            for (iis = ip->isrs()->begin(); iis != ip->isrs()->end() ; ++iis)
                std::cout << (iis == ip->isrs()->begin() ? "":",") << *iis;

            if (ip->err() != RdKafka::ERR_NO_ERROR)
                std::cout << ", " << RdKafka::err2str(ip->err()) << std::endl;
            else
                std::cout << std::endl;
        }
    }
}

bool ASN1_Codec::topic_available( const std::string& topic ) {
    bool r = false;

    RdKafka::Metadata* md;
    RdKafka::ErrorCode err = consumer_ptr->metadata( true, nullptr, &md, 5000 );
    // TODO: Will throw a broker transport error (ERR__TRANSPORT = -195) if the broker is not available.

    if ( err == RdKafka::ERR_NO_ERROR ) {
        RdKafka::Metadata::TopicMetadataIterator it = md->topics()->begin();

        // search for the raw BSM topic.
        while ( it != md->topics()->end() && !r ) {
            // finish when we find it.
            r = ( (*it)->topic() == topic );
            if ( r ) ilogger->info( "Topic: {} found in the kafka metadata.", topic );
            ++it;
        }
        if (!r) ilogger->warn( "Metadata did not contain topic: {}.", topic );

    } else {
        elogger->error( "cannot retrieve consumer metadata with error: {}.", err2str(err) );
    }
    
    return r;
}

void ASN1_Codec::print_configuration() const
{
    std::cout << "# Global config" << "\n";
    std::list<std::string>* conf_list = conf->dump();

    int i = 0;
    for ( auto& v : *conf_list ) {
        if ( i%2==0 ) std::cout << v << " = ";
        else std::cout << v << '\n';
        ++i;
    }

    std::cout << "# Topic config" << "\n";
    conf_list = tconf->dump();
    i = 0;
    for ( auto& v : *conf_list ) {
        if ( i%2==0 ) std::cout << v << " = ";
        else std::cout << v << '\n';
        ++i;
    }

    std::cout << "# Privacy config \n";
    for ( const auto& m : pconf ) {
        std::cout << m.first << " = " << m.second << '\n';
    }
}


/**
 * JMC: asn1 reviewed.
 *
 * The following configuration settings are processed by configure:
 *
 asn1.j2735.kafka.partition
 asn1.j2735.topic.consumer
 asn1.j2735.topic.producer
 asn1.j2735.consumer.timeout.ms

JMC: TODO: All the ASN1C variables that are normally setup do here!

*/
bool ASN1_Codec::configure() {

    if ( optIsSet('v') ) {
        if ( "trace" == optString('v') ) {
            ilogger->set_level( spdlog::level::trace );
        } else if ( "debug" == optString('v') ) {
            ilogger->set_level( spdlog::level::trace );
        } else if ( "info" == optString('v') ) {
            ilogger->set_level( spdlog::level::trace );
        } else if ( "warning" == optString('v') ) {
            ilogger->set_level( spdlog::level::warn );
        } else if ( "error" == optString('v') ) {
            ilogger->set_level( spdlog::level::err );
        } else if ( "critical" == optString('v') ) {
            ilogger->set_level( spdlog::level::critical );
        } else if ( "off" == optString('v') ) {
            ilogger->set_level( spdlog::level::off );
        } else {
            elogger->warn("information logger level was configured but unreadable; using default.");
        }
    } // else it is already set to default.

    ilogger->trace("starting configure()");

    std::string line;
    std::string error_string;
    StrVector pieces;

    // configurations; global and topic (the names in these are fixed)
    conf  = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    tconf = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);

    // must use a configuration file.
    if ( !optIsSet('c') ) {
        elogger->error( "asked to use a configuration file, but option not set." );
        return false;
    }

    const std::string& cfile = optString('c');              // needed for error message.
    ilogger->info("using configuration file: {}", cfile );
    std::ifstream ifs{ cfile };

    if (!ifs) {
        elogger->error("cannot open configuration file: {}", cfile);
        return false;
    }

    while (std::getline( ifs, line )) {
        line = string_utilities::strip( line );
        if ( !line.empty() && line[0] != '#' ) {
            pieces = string_utilities::split( line, '=' );
            bool done = false;
            if (pieces.size() == 2) {
                // in case the user inserted some spaces...
                string_utilities::strip( pieces[0] );
                string_utilities::strip( pieces[1] );
                // some of these configurations are stored in each...?? strange.
                if ( tconf->set(pieces[0], pieces[1], error_string) == RdKafka::Conf::CONF_OK ) {
                    ilogger->info("kafka topic configuration: {} = {}", pieces[0], pieces[1]);
                    done = true;
                }

                if ( conf->set(pieces[0], pieces[1], error_string) == RdKafka::Conf::CONF_OK ) {
                    ilogger->info("kafka configuration: {} = {}", pieces[0], pieces[1]);
                    done = true;
                }

                if ( !done ) { 
                    ilogger->info("ASN1_Codec configuration: {} = {}", pieces[0], pieces[1]);
                    // These configuration options are not expected by Kafka.
                    // Assume there are for the ASN1_Codec.
                    pconf[ pieces[0] ] = pieces[1];
                }

            } else {
                elogger->warn("too many pieces in the configuration file line: {}", line);
            }

        } // otherwise: empty or comment line.
    }

    // All configuration file settings are overridden, if supplied, by CLI options.

    if ( optIsSet('b') ) {
        // broker specified.
        ilogger->info("setting kafka broker to: {}", optString('b'));
        conf->set("metadata.broker.list", optString('b'), error_string);
    } 

    if ( optIsSet('p') ) {
        // number of partitions.
        partition = optInt( 'p' );

    } else {
        auto search = pconf.find("asn1.j2735.kafka.partition");
        if ( search != pconf.end() ) {
            partition = std::stoi(search->second);              // throws.    
        }  // otherwise leave at default; PARTITION_UA
    }

    ilogger->info("kafka partition: {}", partition);

    if ( getOption('g').isSet() && conf->set("group.id", optString('g'), error_string) != RdKafka::Conf::CONF_OK) {
        // NOTE: there are some checks in librdkafka that require this to be present and set.
        elogger->error("kafka error setting configuration parameters group.id h: {}", error_string);
        return false;
    }

    if ( getOption('o').isSet() ) {
        // offset in the consumed stream.
        std::string o = optString( 'o' );

        if (o=="end") {
            offset = RdKafka::Topic::OFFSET_END;
        } else if (o=="beginning") {
            offset = RdKafka::Topic::OFFSET_BEGINNING;
        } else if (o== "stored") {
            offset = RdKafka::Topic::OFFSET_STORED;
        } else {
            offset = strtoll(o.c_str(), NULL, 10);              // throws
        }

        ilogger->info("offset in partition set to byte: {}", o);
    }

    // Do we want to exit if a stream eof is sent.
    exit_eof = getOption('x').isSet();

    if (optIsSet('d') && conf->set("debug", optString('d'), error_string) != RdKafka::Conf::CONF_OK) {
        elogger->error("kafka error setting configuration parameter debug: {}", error_string);
        return false;
    }

    // librdkafka defined configuration.
    conf->set("default_topic_conf", tconf, error_string);

    auto search = pconf.find("asn1.j2735.topic.consumer");
    if ( search != pconf.end() ) {
        consumed_topics.push_back( search->second );
        ilogger->info("consumed topic: {}", search->second);

    } else {
        
        elogger->error("no consumer topic was specified; must fail.");
        return false;
    }

    if (optIsSet('t')) {
        // this is the produced (filtered) topic.
        published_topic_name = optString( 't' );

    } else {
        // maybe it was specified in the configuration file.
        auto search = pconf.find("asn1.j2735.topic.producer");
        if ( search != pconf.end() ) {
            published_topic_name = search->second;
        } else {
            elogger->error("no publisher topic was specified; must fail.");
            return false;
        }
    }

    ilogger->info("published topic: {}", published_topic_name);

    search = pconf.find("asn1.j2735.consumer.timeout.ms");
    if ( search != pconf.end() ) {
        try {
            consumer_timeout = std::stoi( search->second );
        } catch( std::exception& e ) {
            ilogger->info("using the default consumer timeout value.");
        }
    }

    // TODO: This is a GLOBAL THAT NEEDS FIXING; hold over from asn1c
    // This is the input (from the stream) file format.
    iform = INP_PER;

    ilogger->trace("ending configure()");
    return true;
}

bool ASN1_Codec::msg_consume(RdKafka::Message* message, struct xer_buffer* xb ) {

    // For J2735, this points to the asn_TYPE_descriptor_t asn_DEF_MessageFrame structure defined in
    // MessageFrame.c. The asn_TYPE_descriptor_t is in constr_TYPE.h (of the asn1c library)
    // pduType->op is a structure of function pointers that perform operations on the ASN.1.
    // TODO: Put in the class 
    static asn_TYPE_descriptor_t *pduType = &PDU_Type;

    static std::string tsname;
    static RdKafka::MessageTimestamp ts;

    size_t bytes_processed = 0;

    FILE* source;
    void *structure;

    //std::string payload(static_cast<const char*>(message->payload()), message->len());

    // TODO: Could make a library function to take this "message" type and process it..., or just use the raw bytes.
    switch (message->err()) {
        case RdKafka::ERR__TIMED_OUT:
            ilogger->info("Waiting for more BSMs from the ODE producer.");
            break;

        case RdKafka::ERR_NO_ERROR:
            /* Real message */
            msg_recv_count++;
            msg_recv_bytes += message->len();

            ilogger->trace("Read message at byte offset: {} with length {}", message->offset(), message->len() );

            ts = message->timestamp();

            if (ts.type != RdKafka::MessageTimestamp::MSG_TIMESTAMP_NOT_AVAILABLE) {
                if (ts.type == RdKafka::MessageTimestamp::MSG_TIMESTAMP_CREATE_TIME) {
                    tsname = "create time";
                } else if (ts.type == RdKafka::MessageTimestamp::MSG_TIMESTAMP_LOG_APPEND_TIME) {
                    tsname = "log append time";
                } else {
                    tsname = "unknown";
                }

                ilogger->trace("Message timestamp: {}, type: {}", tsname, ts.timestamp);
            }

            if ( message->key() ) {
                ilogger->trace("Message key: {}", *message->key() );
            }

            /** * Strategy:
             *
             * 1. For now just pass an ASCII filename, read that in, open and process those bytes.
             * 2. Next, we will handle the production of binary blobs.
             * 3. Take blocks of the binary, pass them through the library function.
             *
             *
             */

            //            while ( message->len() - bytes_processed > 0 ) {

            ilogger->info("Attempting to decode {} bytes total received {}.", message->len(), msg_recv_bytes );

            // fwrite( (const void*) message->payload(), 1, message->len(), dump_file );

            structure = data_decode_from_buffer(pduType, (const uint8_t *) message->payload(), message->len(), first_block);
            // TODO: Identify this structure.

            if(!structure) {
                ilogger->error("No structure returned from decoding. payload size: {}", message->len());
                elogger->error("No structure returned from decoding. payload size: {}", message->len());
                return false;
            }

            first_block = false;

            // JMC: Dump these to a string stream instead or directly to the kafka buffer.
            if( xer_buf( static_cast<void *>(xb), pduType, structure) ) {
                ilogger->error("Cannot convert the block into XML.");
                elogger->error("Cannot convert the block into XML.");
                return false;
            }

            ilogger->info( "Finished decode/encode operation for {} bytes.", xb->buffer_size );
            // Good data exit point.
            return true;
            break;

        case RdKafka::ERR__PARTITION_EOF:
            ilogger->info("ODE BSM consumer partition end of file, but ASN1_Codec still alive.");
            if (exit_eof) {
                eof_cnt++;

                if (eof_cnt == partition_cnt) {
                    ilogger->info("EOF reached for all {} partition(s)", partition_cnt);
                    data_available = false;
                }
            }
            break;

        case RdKafka::ERR__UNKNOWN_TOPIC:
            elogger->error("cannot consume due to an UNKNOWN consumer topic: {}", message->errstr());

        case RdKafka::ERR__UNKNOWN_PARTITION:
            elogger->error("cannot consume due to an UNKNOWN consumer partition: {}", message->errstr());
            data_available = false;
            break;

        default:
            elogger->error("cannot consume due to an error: {}", message->errstr());
            data_available = false;
    }

    return false;
}

/**
 * JMC: asn1 reviewed.
 */
bool ASN1_Codec::launch_producer()
{
    std::string error_string;

    producer_ptr = std::shared_ptr<RdKafka::Producer>( RdKafka::Producer::create(conf, error_string) );
    if ( !producer_ptr ) {
        elogger->critical("Failed to create producer with error: {}.", error_string );
        return false;
    }

    published_topic_ptr = std::shared_ptr<RdKafka::Topic>( RdKafka::Topic::create(producer_ptr.get(), published_topic_name, tconf, error_string) );
    if ( !published_topic_ptr ) {
        elogger->critical("Failed to create topic: {}. Error: {}.", published_topic_name, error_string );
        return false;
    } 

    ilogger->info("Producer: {} created using topic: {}.", producer_ptr->name(), published_topic_name);
    return true;
}

/**
 * JMC: asn1 reviewed.
 */
bool ASN1_Codec::launch_consumer()
{
    std::string error_string;

    consumer_ptr = std::shared_ptr<RdKafka::KafkaConsumer>( RdKafka::KafkaConsumer::create(conf, error_string) );
    if (!consumer_ptr) {
        elogger->critical("Failed to create consumer with error: {}",  error_string );
        return false;
    }

    // wait on the topics we specified to become available for subscription.
    // loop terminates with a signal (CTRL-C) or when all the topics are available.
    int tcount = 0;
    for ( auto& topic : consumed_topics ) {
        while ( data_available && tcount < consumed_topics.size() ) {
            if ( topic_available(topic) ) {
                ilogger->trace("Consumer topic: {} is available.", topic);
                // count it and attempt to get the next one if it exists.
                ++tcount;
                break;
            }
            // topic is not available, wait for a second or two.
            std::this_thread::sleep_for( std::chrono::milliseconds( 1500 ) );
            ilogger->trace("Waiting for needed consumer topic: {}.", topic);
        }
    }

    if ( tcount == consumed_topics.size() ) {
        // all the needed topics are available for subscription.
        RdKafka::ErrorCode status = consumer_ptr->subscribe(consumed_topics);
        if (status) {
            elogger->critical("Failed to subscribe to {} topics. Error: {}.", consumed_topics.size(), RdKafka::err2str(status) );
            return false;
        }
    } else {
        ilogger->warn("User cancelled ASN1_Codec while waiting for topics to become available.");
        return false;
    }

    std::ostringstream osbuf{};
    for ( auto& topic : consumed_topics ) {
        if ( osbuf.tellp() != 0 ) osbuf << ", ";
        osbuf << topic;
    }

    ilogger->info("Consumer: {} created using topics: {}.", consumer_ptr->name(), osbuf.str());
    return true;
}

bool ASN1_Codec::make_loggers( bool remove_files )
{
    // defaults.
    std::string path{ "logs/" };
    std::string ilogname{ "log.info" };
    std::string elogname{ "log.error" };

    if (getOption('D').hasArg()) {
        // replace default
        path = getOption('D').argument();
        if ( path.back() != '/' ) {
            path += '/';
        }
    }

    // if the directory specified doesn't exist, then make it.
    if (!dirExists( path )) {
#ifndef _MSC_VER
        if (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH ) != 0)   // linux
#elif _MSC_VER 
        if (_mkdir(path.c_str()) != 0)                                          // windows
#else
                                                                                // some other strange os...
#endif
        {
            std::cerr << "Error making the logging directory.\n";
            return false;
        }
    }
    
    // ilog check for user-defined file locations and names.
    if (getOption('i').hasArg()) {
        // replace default.
        ilogname = string_utilities::basename<std::string>( getOption('i').argument() );
    }

    if (getOption('e').hasArg()) {
        // replace default.
        elogname = string_utilities::basename<std::string>( getOption('e').argument() );
    }
    
    ilogname = path + ilogname;
    elogname = path + elogname;

    if ( remove_files && fileExists( ilogname ) ) {
        if ( std::remove( ilogname.c_str() ) != 0 ) {
            std::cerr << "Error removing the previous information log file.\n";
            return false;
        }
    }

    if ( remove_files && fileExists( elogname ) ) {
        if ( std::remove( elogname.c_str() ) != 0 ) {
            std::cerr << "Error removing the previous error log file.\n";
            return false;
        }
    }

    // setup information logger.
    ilogger = spdlog::rotating_logger_mt("ilog", ilogname, ilogsize, ilognum);
    ilogger->set_pattern("[%C%m%d %H:%M:%S.%f] [%l] %v");
    ilogger->set_level( iloglevel );

    // setup error logger.
    elogger = spdlog::rotating_logger_mt("elog", elogname, elogsize, elognum);
    elogger->set_level( iloglevel );
    elogger->set_pattern("[%C%m%d %H:%M:%S.%f] [%l] %v");
    return true;
}

int ASN1_Codec::operator()(void) {

    std::string error_string;
    RdKafka::ErrorCode status;

    signal(SIGINT, sigterm);
    signal(SIGTERM, sigterm);
    
    try {

        // throws for a couple of options.
        if ( !configure() ) return EXIT_FAILURE;

    } catch ( std::exception& e ) {

        // don't use logger in case we cannot configure it correctly.
        std::cerr << "Fatal Exception: " << e.what() << '\n';
        return EXIT_FAILURE;
    }

    if ( !launch_consumer() ) return false;
    if ( !launch_producer() ) return false;

    // TODO: Put this in the class.
    struct xer_buffer xb = {0, 0, 0};

    // consume-produce loop.
    while (data_available) {

        // reset the write point to the start of the buffer.
        xb.buffer_size = 0;

        std::unique_ptr<RdKafka::Message> msg{ consumer_ptr->consume( consumer_timeout ) };

        if ( msg->len() > 0 && msg_consume(msg.get(), &xb) ) {

            std::cerr << msg->len() << " bytes consumed from topic: " << consumed_topics[0] << '\n';
            status = producer_ptr->produce(published_topic_ptr.get(), partition, RdKafka::Producer::RK_MSG_COPY, (void *)xb.buffer, xb.buffer_size, NULL, NULL);

            if (status != RdKafka::ERR_NO_ERROR) {
                elogger->error("Failure of XER encoding: {}", RdKafka::err2str( status ));

            } else {
                // successfully sent; update counters.
                msg_send_count++;
                msg_send_bytes += xb.buffer_size;
                ilogger->trace("Success of XER encoding.");
                std::cerr << xb.buffer_size << " bytes produced to topic: " << published_topic_ptr->name() << '\n';
            }

        } 

        // NOTE: good for troubleshooting, but bad for performance.
        elogger->flush();
        ilogger->flush();
    }

    ilogger->info("ASN1_Codec operations complete; shutting down...");
    ilogger->info("ASN1_Codec consumed  : {} blocks and {} bytes", msg_recv_count, msg_recv_bytes);
    ilogger->info("ASN1_Codec published : {} blocks and {} bytes", msg_send_count, msg_send_bytes);

    std::cerr << "ASN1_Codec operations complete; shutting down...\n";
    std::cerr << "ASN1_Codec consumed   : " << msg_recv_count << " blocks and " << msg_recv_bytes << " bytes\n";
    std::cerr << "ASN1_Codec published  : " << msg_send_count << " blocks and " << msg_send_bytes << " bytes\n";
    return EXIT_SUCCESS;
}

#ifndef _ASN1_CODEC_TESTS

int main( int argc, char* argv[] )
{
    ASN1_Codec asn1_codec{"ASN1_Codec","ASN1 Processing Module"};

    asn1_codec.addOption( 'c', "config", "Configuration file name and path.", true );
    asn1_codec.addOption( 'C', "config-check", "Check the configuration file contents and output the settings.", false );
    asn1_codec.addOption( 't', "produce-topic", "The name of the topic to produce.", true );
    asn1_codec.addOption( 'p', "partition", "Consumer topic partition from which to read.", true );
    asn1_codec.addOption( 'g', "group", "Consumer group identifier", true );
    asn1_codec.addOption( 'b', "broker", "Broker address (localhost:9092)", true );
    asn1_codec.addOption( 'o', "offset", "Byte offset to start reading in the consumed topic.", true );
    asn1_codec.addOption( 'x', "exit", "Exit consumer when last message in partition has been received.", false );
    asn1_codec.addOption( 'd', "debug", "debug level.", true );
    asn1_codec.addOption( 'v', "log-level", "The info log level [trace,debug,info,warning,error,critical,off]", true );
    asn1_codec.addOption( 'D', "log-dir", "Directory for the log files.", true );
    asn1_codec.addOption( 'R', "log-rm", "Remove specified/default log files if they exist.", false );
    asn1_codec.addOption( 'i', "ilog", "Information log file name.", true );
    asn1_codec.addOption( 'e', "elog", "Error log file name.", true );
    asn1_codec.addOption( 'h', "help", "print out some help" );


    // debug for ASN.1
    opt_debug = 0;

    if (!asn1_codec.parseArgs(argc, argv)) {
        asn1_codec.usage();
        std::exit( EXIT_FAILURE );
    }

    if (asn1_codec.optIsSet('h')) {
        asn1_codec.help();
        std::exit( EXIT_SUCCESS );
    }

    // can set levels if needed here.
    if ( !asn1_codec.make_loggers( asn1_codec.optIsSet('R') )) {
        std::exit( EXIT_FAILURE );
    }

    // configuration check.
    if (asn1_codec.optIsSet('C')) {
        try {
            if (asn1_codec.configure()) {
                asn1_codec.print_configuration();
                std::exit( EXIT_SUCCESS );
            } else {
                std::cerr << "Current configuration settings do not work.\n";
                asn1_codec.ilogger->error( "current configuration settings do not work; exiting." );
                std::exit( EXIT_FAILURE );
            }
        } catch ( std::exception& e ) {
            std::cerr << "Fatal Exception: " << e.what() << '\n';
            asn1_codec.elogger->error( "exception: {}", e.what() );
            std::exit( EXIT_FAILURE );
        }
    }



    // The module will run and when it terminates return an appropriate error code.
    std::exit( asn1_codec.run() );
}

#endif