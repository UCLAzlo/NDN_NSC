#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/v2/validator.hpp>
#include <ndn-cxx/security/v2/validation-callback.hpp>
#include <ndn-cxx/security/v2/certificate-fetcher-offline.hpp>
#include <boost/asio/io_service.hpp>

#include <functional>
#include <fstream>
#include <string>
#include <iostream>

using namespace ndn;

class rpcConsumer
{
public:
    rpcConsumer(char *ccInput)
        : m_face(m_ioService),
          m_scheduler(m_ioService),
          CCNUM(ccInput)
    {
        rpcCall = 0;
    }

    void run()
    {
        std::cerr << "Attempting to rpc call" << std::endl;
        std::cerr << "------------------------" << std::endl;

        m_face.registerPrefix(INPUT_NAMESPACE,
                              RegisterPrefixSuccessCallback(),
                              bind(&rpcConsumer::onRegisterFailed, this, _1, _2));

        m_scheduler.schedule(1_ns, bind(&rpcConsumer::pubAndNotify, this));
        m_ioService.run();
    }

    void pubAndNotify()
    {
        std::cerr << "Attempting to publish data" << std::endl;
        std::cerr << "------------------------" << std::endl;

        //publish data
        int publishNum = publishInput();

        //send notification interest
        sendNotification(publishNum);
    }

private:
    boost::asio::io_service m_ioService;
    Face m_face;
    Scheduler m_scheduler;
    KeyChain m_keyChain;
    int rpcCall;
    const std::string CCNUM;
    const std::string CONSUMER_IDENTITY = "/pos/device1";
    const std::string PRODUCER_IDENTITY = "/eshop";
    const std::string PRODUCER_FUNC_NAME = "/eshop/cardchecker/function";
    const std::string INPUT_NAMESPACE = CONSUMER_IDENTITY + "/cardchecker/inputs/";
    const std::string APP_NACK = "APP_NACK";

    //Publish Input data for future RPC Producer to retrieve
    int publishInput()
    {
        int publishNum = ++rpcCall;
        m_face.setInterestFilter(INPUT_NAMESPACE + std::to_string(publishNum),
                                 bind(&rpcConsumer::onInterestForInput, this, _1, _2));

        std::cerr << "LISTENING TO " << INPUT_NAMESPACE + std::to_string(publishNum) << std::endl;
        std::cerr << "RPC Call is at " << rpcCall << std::endl;
        std::cerr << "------------------------" << std::endl;
        return publishNum;
    }

    //Send a notification to the RPC Producer to initiate RPC Call
    void sendNotification(int publishNum)
    {
        Interest interest = createInterest(PRODUCER_FUNC_NAME, true, true);
        addInterestParameterString(INPUT_NAMESPACE + std::to_string(publishNum), interest);
        m_keyChain.sign(interest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
        m_face.expressInterest(interest,
                               bind(&rpcConsumer::onNotificationData, this, _1, _2),
                               bind(&rpcConsumer::onNack, this, _1, _2),
                               bind(&rpcConsumer::onTimeout, this, _1));

        std::cerr << "Sending Notification Interest\n"
                  << PRODUCER_FUNC_NAME << std::endl;
        std::cerr << "------------------------" << std::endl;
    }

    //Acknowledge that Producer received RPC Notification
    void onNotificationData(const Interest &, const Data &data)
    {
        std::cerr << "Received Acknowledgement to Notification:" << std::endl;
        std::cerr << "------------------------" << std::endl;
    }

    //Respond with original Input Data
    void onInterestForInput(const InterestFilter &, const Interest &interest)
    {
        std::cerr << "Received an interest for credit card number input" << std::endl;
        if (verifyInterestSignature(interest, PRODUCER_IDENTITY))
        {
            std::cerr << "Sending Credit Card Number Data as Published Earlier" << std::endl;
            std::cerr << "------------------------" << std::endl;

            auto data = createData(interest.getName(), CCNUM, CONSUMER_IDENTITY);
            m_face.put(*data);

            std::string resultName = extractInterestParam(interest);
            sendInterestForResult(resultName);
        }
    }

    //Request results of RPC Call from location provided as Interest Parameters
    void sendInterestForResult(std::string resultName)
    {
        Interest interest = createInterest(resultName, false, true);
        m_keyChain.sign(interest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
        m_face.expressInterest(interest,
                               bind(&rpcConsumer::onResultData, this, _1, _2),
                               bind(&rpcConsumer::onNack, this, _1, _2),
                               bind(&rpcConsumer::onTimeout, this, _1));

        std::cerr << "Sending Interest for final Result Data " << std::endl;
        std::cerr << resultName << std::endl;
        std::cerr << "------------------------" << std::endl;
    }

    //Print result Data
    void onResultData(const Interest &interest, const Data &data)
    {
        std::cerr << "Received Result Data from Producer" << std::endl;

        if (verifyDataSignature(data, PRODUCER_IDENTITY))
        {
            std::string ccResult(reinterpret_cast<const char *>(data.getContent().value()));

            //check for application NACK
            if (isAppNACK(ccResult))
            {

                std::string newResultName = ccResult.substr(APP_NACK.length(), ccResult.length() - APP_NACK.length());
                Interest delayInterest = createInterest(newResultName, false, true);
                m_keyChain.sign(delayInterest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
                m_face.expressInterest(delayInterest,
                                       bind(&rpcConsumer::onResultData, this, _1, _2),
                                       bind(&rpcConsumer::onNack, this, _1, _2),
                                       bind(&rpcConsumer::onTimeout, this, _1));

                std::cerr << "Received Delay message, now retrying " << newResultName << std::endl;
            }
            else
            {
                std::cerr << "Result of RPC Call, Credit Card Number is: " << ccResult << std::endl;
                std::cerr << "------------------------" << std::endl;
                m_ioService.stop();
            }
        }
    }

    bool isAppNACK(std::string dataContent)
    {
        std::size_t found = dataContent.find(APP_NACK);
        if (found == 0)
            return true;
        else
            return false;
    }

    //create an Interest packet with specified values
    Interest createInterest(std::string name, bool canBePrefix, bool mustBeFresh)
    {
        Name interestName(name);
        Interest interest(interestName);
        interest.setCanBePrefix(canBePrefix);
        interest.setMustBeFresh(mustBeFresh);

        return interest;
    }

    //extract Interest Parameter as String
    std::string extractInterestParam(const Interest &interest)
    {
        std::string interestParam(reinterpret_cast<const char *>(interest.getApplicationParameters().value()));
        return interestParam;
    }

    //Add a string as an Interest Parameter
    void addInterestParameterString(std::string params, Interest &interest)
    {
        const uint8_t *params_uint = reinterpret_cast<const uint8_t *>(&params[0]);
        interest.setApplicationParameters(params_uint, params.length() + 1);
    }

    //create a Data packet with specified values
    std::shared_ptr<ndn::Data> createData(const ndn::Name dataName, std::string content, std::string identity)
    {
        auto data = make_shared<Data>(dataName);
        data->setFreshnessPeriod(5_ms);
        data->setContent(reinterpret_cast<const uint8_t *>(content.c_str()), content.length() + 1);
        m_keyChain.sign(*data, security::signingByIdentity(Name(identity)));

        return data;
    }

    //Retrieves Key for a specific identity
    ndn::security::pib::Key getKeyForIdentity(std::string identity)
    {
        const auto &pib = m_keyChain.getPib();
        const auto &verifyIdentity = pib.getIdentity(Name(identity));
        return verifyIdentity.getDefaultKey();
    }

    //Signature Verification Functions for Interest
    bool verifyInterestSignature(const Interest &interest, std::string identity)
    {
        if (security::verifySignature(interest, getKeyForIdentity(identity)))
        {
            std::cerr << "Interest Signature - Verified" << std::endl;
            return true;
        }
        else
        {
            std::cerr << "Interest Signature - ERROR, can't verify" << std::endl;
            return false;
        }
    }

    //Signature Verification Functions for Data
    bool verifyDataSignature(const Data &data, std::string identity)
    {
        if (security::verifySignature(data, getKeyForIdentity(identity)))
        {
            std::cerr << "Data Signature - Verified" << std::endl;
            return true;
        }
        else
        {
            std::cerr << "Data Signature - ERROR, can't verify" << std::endl;
            return false;
        }
    }

    //Boilerplate NACK, Timeout, Failure to Register
    void onNack(const Interest &, const lp::Nack &nack) const
    {
        std::cerr << "Received Nack with reason " << nack.getReason() << std::endl;
        std::cerr << "------------------------" << std::endl;
    }

    void onTimeout(const Interest &interest) const
    {
        std::cerr << "Timeout for " << interest << std::endl;
        std::cerr << "------------------------" << std::endl;
    }

    void onRegisterFailed(const Name &prefix, const std::string &reason)
    {
        std::cerr << "ERROR: Failed to register prefix '" << prefix
                  << "' with the local forwarder (" << reason << ")" << std::endl;
        std::cerr << "------------------------" << std::endl;
        m_face.shutdown();
    }
};

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2)
        {
            std::cerr << "Usage: ./consumer <credit card num>" << std::endl;
            exit(1);
        }
        rpcConsumer consumer1(argv[1]);
        consumer1.run();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        std::cerr << "------------------------" << std::endl;
        return 1;
    }
}