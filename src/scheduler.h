/*************************************************************************\
 * AdapterRemoval - cleaning next-generation sequencing reads            *
 *                                                                       *
 * Copyright (C) 2015 by Mikkel Schubert - mikkelsch@gmail.com           *
 *                                                                       *
 * If you use the program, please cite the paper:                        *
 * S. Lindgreen (2012): AdapterRemoval: Easy Cleaning of Next Generation *
 * Sequencing Reads, BMC Research Notes, 5:337                           *
 * http://www.biomedcentral.com/1756-0500/5/337/                         *
 *                                                                       *
 * This program is free software: you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation, either version 3 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>. *
\*************************************************************************/
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "threads.h"


struct data_chunk;
struct scheduler_step;


/**
 * This exception may be thrown by a task to abort the thread; error-messages
 * are assumed to have already been printed by the thrower, and no furher
 * messages are printed.
 */
class thread_abort : public thread_error
{
public:
    thread_abort();
};


/**
 * Base-class for data-chunks produced, processed and consumed by a pipeline.
 */
class analytical_chunk
{
public:
    /** Constructor; does nothing. */
    analytical_chunk();

    /** Destructor; does nothing. */
    virtual ~analytical_chunk();
};


typedef std::pair<size_t, analytical_chunk*> chunk_pair;
typedef std::list<chunk_pair> chunk_list;


/**
 * Sink for generating in-memory data-sinks of type T on demand, and reducing
 * these to a single result upon completion of analyses. Using this class
 * allows multiple threads to collect summary statistics, while the final
 * consumer sees only a single statistics object.
 *
 * The class T must implement the += operator, to allow the reduction of sinks.
 */
template <typename T>
class statistics_sink
{
public:
    /** Constructor; does nothing. */
    statistics_sink();

    /** Destructor; deletes any remaining sinks. */
    virtual ~statistics_sink();

    /** Returns unused sink, or a new sink if no unused sinks are available. */
    virtual T* get_sink();

    /** Return a sink after it has been used. */
    virtual void return_sink(T* ptr);

    /**
     * Return a single sink that is the sum of all sink objects, consuming (and
     * freeing) all sink objects generated by and returned to the sink.
     */
    virtual T* finalize();

protected:
    /** Returns a new sink object; to be implemented in subclasses. */
    virtual T* new_sink() const = 0;

private:
    typedef std::list<T*> sink_list;
    typedef typename sink_list::iterator sink_list_iter;
    typedef typename sink_list::const_iterator sink_list_citer;

    //! Lock used to control access to sink lists
    mutex m_sinks_lock;
    //! List of inactive sinks
    sink_list m_sinks;
};


/**
 * Base class for analytical steps in a pipeline.
 *
 * Each step must implement the 'process' function as described below; note
 * that this function may be called simutanously by multiple threads, and that
 * thread-safe storage (e.g. statistics_sink) must be used for writable
 * resources used by the step.
 */
class analytical_step
{
public:
    enum ordering {
        //! Data must be consumed in the input order
        ordered,
        //! Data may be consumed in any order
        unordered
    };

    /**
     * @param step_order Indicates the expected ordering of chunks; processing
     *                   steps are typically unordered, while IO is typically
     *                   ordered in order to ensure that output order matches
     *                   input order.
     * @param file_io Indicates if the step involves the use of file IO.
     */
    analytical_step(ordering step_order, bool file_io = false);

    /** Destructor; does nothing in base class. **/
    virtual ~analytical_step();


    /**
     * Function called by pipeline to generate / process / consume data chunks.
     *
     * Initially, the first step in the pipeline will recieve NULL; during
     * subsequent cycles, the pipeline will return the value output from the
     * last step to the initial step, which may re-use it to avoid allocations;
     * if this is not done, the chunk must be freed by the first step.
     *
     * Best performance is therefore obtained if the chunk contains buffers
     * for all steps, and these can be re-used across cycles, thereby reducing
     * the number of (de)allocations that must be performed.
     *
     * To terminate the pipeline, the first step must cease to return chunks;
     * however, any other step MUST return valid chunks, even if no input data
     * was provided. This is to ensure that tracking of chunk ordering can be
     * maintained across steps.
     *
     * The only exceptions to this rule are steps which ONLY has unordered
     * downstream steps (that is, no chunk generated by this step will be
     * processed by an ordered step later in the pipeline).
     */
    virtual chunk_list process(analytical_chunk* chunk) = 0;

    /**
     * Called once the pipeline has been run to completion; this function is
     * called on nodes in the same order as the pipeline.
     */
    virtual void finalize();


    /** Returns the expected ordering (ordered / unordered) for input data. **/
    ordering get_ordering() const;

    /** Returns true if the step involves file IO. */
    bool file_io() const;

private:
    //! Stores the ordering of data chunks expected by the step
    ordering m_step_order;
    //! True if the step involves file IO (read and / or writes)
    bool m_file_io;
};


/**
 * Multithreaded scheduler.
 *
 * See 'analytical_step' for information on implementing analyses.
 */
class scheduler
{
public:
    /** Constructor. */
    scheduler();

    /** Frees any object passed via 'add_step'. **/
    ~scheduler();

    /**
     * Adds a step to the pipeline.
     *
     * @param step_id Unique ID of current step; cannot be used twice.
     * @param step A analytical step; is deleted when scheduler is destroyed.
     *
     * The ID specified here is specified as the first value of 'chunk_pair's
     * in order to determine to which analytical step a chunk is assigned.
     **/
    void add_step(size_t step_id, analytical_step* step);

    /** Runs the pipeline with n threads; return false on error. */
    bool run(int nthreads);

private:
    typedef std::list<scheduler_step*> runables;
    typedef std::vector<scheduler_step*> pipeline;
    typedef std::vector<pthread_t> thread_vector;

    //! Not implemented
    scheduler(const scheduler&);
    //! Not implemented
    scheduler& operator=(const scheduler&);

    /** Wrapper function which calls do_run on the provided thread. */
    static void* run_wrapper(void*);
    /** Work function; invoked by each thread. */
    void* do_run();

    /** Initializes n threads, returning false if any errors occured. */
    bool initialize_threads(int nthreads);
    /** Sends a number of signals corresponding to the number of threads. */
    void signal_threads();
    /** Joins all threads, returning false if any errors occured. */
    bool join_threads();

    /** Executes an analytical step. */
    void execute_analytical_step(scheduler_step* step);
    /** Attempts to queue an analytical step given a current chunk. */
    void queue_analytical_step(scheduler_step* step, size_t current);

    //! Analytical steps
    pipeline m_steps;
    //! Lock set when the scheduler is running
    mutex m_running;
    //! Set to indicate if errors have occured
    volatile bool m_errors;
    //! Condition used to signal the (potential) availability of work
    conditional m_condition;

    //! Counter used for sequential processing of data
    size_t m_chunk_counter;
    //! List of current threads, excluding the main thread
    thread_vector m_threads;

    //! Lock used to control access to chunks
    mutex m_queue_lock;
    //! Queue used for currently runnable steps involving only calculations
    runables m_queue_calc;
    //! Queue used for currently runnable steps involving IO
    runables m_queue_io;
    //! Indicates if a thread is doing IO; access control through 'm_queue_lock'
    bool m_io_active;
    //! Count of currently live chunks
    size_t m_live_chunks;
};


///////////////////////////////////////////////////////////////////////////////
// Implementations for 'statistics_sink'

template <typename T>
statistics_sink<T>::statistics_sink()
  : m_sinks_lock()
  , m_sinks()
{
}


template <typename T>
statistics_sink<T>::~statistics_sink()
{
    for (sink_list_iter it = m_sinks.begin(); it != m_sinks.end(); ++it) {
        delete *it;
    }
}


template <typename T>
T* statistics_sink<T>::get_sink()
{
    mutex_locker lock(m_sinks_lock);
    if (m_sinks.empty()) {
        return new_sink();
    }

    T* ptr = m_sinks.front();
    m_sinks.pop_front();

    return ptr;
}


template <typename T>
void statistics_sink<T>::return_sink(T* ptr)
{
    mutex_locker lock(m_sinks_lock);
    m_sinks.push_back(ptr);
}


template <typename T>
T* statistics_sink<T>::finalize()
{
    mutex_locker lock(m_sinks_lock);
    if (m_sinks.empty()) {
        return new_sink();
    }

    std::auto_ptr<T> result(m_sinks.back());
    m_sinks.pop_back();

    while (!m_sinks.empty()) {
        *result += *m_sinks.back();
        delete m_sinks.back();
        m_sinks.pop_back();
    }

    return result.release();
}


///////////////////////////////////////////////////////////////////////////////
// Implementations for 'analytical_step'

inline void analytical_step::finalize()
{
}


inline analytical_step::ordering analytical_step::get_ordering() const
{
    return m_step_order;
}


inline bool analytical_step::file_io() const
{
    return m_file_io;
}

#endif
