#include <click/config.h>
#include <click/glue.hh>
#include <clicknet/tcp.h>
#include "retransmissiontiming.hh"
#include "tcpretransmitter.hh"

CLICK_DECLS

RetransmissionTiming::RetransmissionTiming()
{
    computeClockGranularity();
    srtt = 0;
    rttvar = 0;
    rto = 3000; // RFC 1122
    measureInProgress = false;
    owner = NULL;
}

RetransmissionTiming::~RetransmissionTiming()
{
    stopTimer();
}

void RetransmissionTiming::computeClockGranularity()
{
    // Determine the smallest amount of time measurable for the RTT values

    // We first check the epsilon of the timestamps
    uint32_t epsilon = Timestamp::epsilon().msec();

    // We then check the precision of the timers
    uint32_t timerAdjustment = timer.adjustment().msec();

    // We select the highest value between both of them
    if(epsilon < timerAdjustment)
        clockGranularity = timerAdjustment;
    else
        clockGranularity = epsilon;
}

void RetransmissionTiming::initTimer(struct fcb* fcb, TCPRetransmitter *retransmitter)
{
    owner = retransmitter;
    timer.initialize((Element*)retransmitter);
    // Assign the callback of the timer
    // Give it a pointer to the fcb so that when the timer fires, we
    // can access it
    timerData.retransmitter = retransmitter;
    timerData.fcb = fcb;
    timer.assign(RetransmissionTiming::timerFired, (void*)&timerData);
}

bool RetransmissionTiming::isTimerInitialized()
{
    return timer.initialized();
}

bool RetransmissionTiming::startRTTMeasure(uint32_t seq)
{
    if(measureInProgress)
        return false;

    measureInProgress = true;
    rttSeq = seq;
    measureStartTime.assign_now();

    return true;
}

bool RetransmissionTiming::signalAck(struct fcb *fcb, uint32_t ack)
{
    if(owner != NULL)
        owner->signalAck(fcb, ack);

    if(!measureInProgress)
        return false;

    if(SEQ_GT(ack, rttSeq))
    {
        // The ACK is greater than the sequence number used to start measure
        // It means the destination received the data used to estimate
        // the RTT
        measureEndTime.assign_now();
        measureInProgress = false;

        // Compute the RTT
        measureEndTime -= measureStartTime;
        uint32_t rtt = measureEndTime.msecval();


        if(srtt == 0)
        {
            // First measure
            srtt = rtt;
            rttvar = rtt / 2;
        }
        else
        {
            // Subsequent measures

            // We use float variables during the computations
            float rttvarFloat = (float)rttvar;
            float srttFloat = (float)srtt;
            float rttFloat = (float)rtt;

            float rttAbs = 0;
            if(srttFloat > rttFloat)
                rttAbs = srttFloat - rttFloat;
            else
                rttAbs = rttFloat - srttFloat;

            rttvarFloat = (1.0 - BETA) * rttvarFloat + BETA * rttAbs;
            srttFloat = (1.0 - ALPHA) * srttFloat + ALPHA * rttFloat;

            // We do not need a submillisecond precision so we can use integer
            // values
            rttvar = (uint32_t) rttvarFloat;
            srtt = (uint32_t) srttFloat;
        }

        // Computing the RTO
        rto = srtt;
        uint32_t rttvarFactor = K * rttvar;
        if(clockGranularity > rttvarFactor)
            rto += clockGranularity;
        else
            rto += rttvarFactor;

        click_chatter("RTT measured: %u, RTO: %u", srtt, rto);

        checkRTOMinValue();
        checkRTOMaxValue();

        return true;
    }

    return false;
}

bool RetransmissionTiming::signalRetransmission(uint32_t expectedAck)
{
    if(!measureInProgress)
        return false;

    // If we retransmit data with an expected ACK greater than the sequence
    // number we use for the measure, it means we are retransmitting the data
    // used for the measure. Thus, it can't be used to estimate the RTT
    // (Karn's algorithm)
    if(SEQ_GT(expectedAck, rttSeq))
    {
        // Stop the measure
        measureInProgress = false;

        return true;
    }

    return false;
}

bool RetransmissionTiming::isMeasureInProgress()
{
    return measureInProgress;
}

bool RetransmissionTiming::startTimer()
{
    if(!isTimerInitialized() || isTimerRunning())
        return false;

    timer.schedule_after_msec(rto);

    click_chatter("Timer starting (%u)", rto);

    return true;
}

bool RetransmissionTiming::startTimerDoubleRTO()
{
    if(!isTimerInitialized() || isTimerRunning())
        return false;

    rto *= 2;
    checkRTOMaxValue();
    timer.schedule_after_msec(rto);

    click_chatter("Timer starting with double RTO (%u)", rto);

    return true;
}

bool RetransmissionTiming::stopTimer()
{
    if(!isTimerInitialized() || !isTimerRunning())
        return false;

    timer.unschedule();

    click_chatter("Timer stopped");

    return true;
}

bool RetransmissionTiming::restartTimer()
{
    stopTimer();

    if(!startTimer())
        return false;

    return true;
}

bool RetransmissionTiming::isTimerRunning()
{
    if(!isTimerInitialized())
        return false;

    return timer.scheduled();
}

void RetransmissionTiming::checkRTOMaxValue()
{
    // Max 60 seconds for the RTO
    if(rto > 60000)
        rto = 60000;
}

void RetransmissionTiming::checkRTOMinValue()
{
    // RTO should be at least one second (RFC 1122)
    if(rto < 1000)
        rto = 1000;
}

void RetransmissionTiming::timerFired(Timer *timer, void *data)
{
    struct fcb *fcb = (struct fcb*)((struct retransmissionTimerData*)data)->fcb;
    TCPRetransmitter *retransmitter = (TCPRetransmitter*)((struct retransmissionTimerData*)data)->retransmitter;
    retransmitter->retransmissionTimerFired(fcb);
}

CLICK_ENDDECLS

ELEMENT_PROVIDES(RetransmissionTiming)
