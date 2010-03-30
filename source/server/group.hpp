//  group
//  Copyright (C) 2008, 2009 Tim Blechmann
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  Boston, MA 02111-1307, USA.

#ifndef SERVER_GROUP_HPP
#define SERVER_GROUP_HPP

#include <set>

#include "memory_pool.hpp"
#include "node_types.hpp"
#include "utilities/exists.hpp"
#include "dsp_thread_queue_node.hpp"

namespace nova
{

class synth;

typedef nova::dsp_queue_node<rt_pool_allocator<void*> > queue_node;
typedef nova::dsp_thread_queue_item<dsp_queue_node<rt_pool_allocator<void*> >,
                                    rt_pool_allocator<void*> > thread_queue_item;
typedef nova::dsp_thread_queue<dsp_queue_node<rt_pool_allocator<void*> >,
                               rt_pool_allocator<void*> > thread_queue;

class abstract_group:
    public server_node
{
public:
    typedef thread_queue_item::successor_list successor_container;

protected:
    server_node_list child_nodes;
    const bool is_parallel_;

    abstract_group(int node_id, bool is_parallel):
        server_node(node_id, false), is_parallel_(is_parallel), child_synths_(0), child_groups_(0)
    {}

public:
    virtual ~abstract_group(void);

    bool is_parallel(void) const
    {
        return is_parallel_;
    }

protected:
    virtual successor_container fill_queue_recursive(thread_queue & queue,
                                                     successor_container,
                                                     int activation_limit) = 0;
    friend class group;
    friend class parallel_group;

public:
    /* count the tail nodes to get activation count */
    virtual int tail_nodes(void) const = 0;

    /* @{ */
    /** pause/resume handling (set pause/resume on children)  */
    virtual void pause(void);
    virtual void resume(void);
    /* @} */

    /* @{ */
    /** child management  */
    void add_child(server_node * node);
    virtual void add_child(server_node * node, node_position_constraint const &) = 0;
    virtual void add_child(server_node * node, node_position) = 0;

    bool has_child(server_node * node);

    bool empty(void) const
    {
        return child_nodes.empty();
    }

    /* returns true, if this or any of the child group has synth children */
    bool has_synth_children(void) const
    {
        for (server_node_list::const_iterator it = child_nodes.begin(); it != child_nodes.end(); ++it)
        {
            if (it->is_synth())
                return true;
            const abstract_group * group = static_cast<const abstract_group*>(&*it);
            if (group->has_synth_children())
                return true;
        }
        return false;
    }

    /** number of direct children */
    std::size_t child_count(void) const
    {
        return child_synths_ + child_groups_;
    }

    /** number of child synths and groups */
    std::pair<std::size_t, std::size_t> child_count_deep(void) const
    {
        std::size_t synths = child_synths_;
        std::size_t groups = child_groups_;

        for (server_node_list::const_iterator it = child_nodes.begin(); it != child_nodes.end(); ++it)
        {
            if (it->is_group()) {
                std::size_t recursive_synths, recursive_groups;
                const abstract_group * group = static_cast<const abstract_group*>(&*it);
                boost::tie(recursive_synths, recursive_groups) = group->child_count_deep();
                groups += recursive_groups;
                synths += recursive_synths;
            }
        }
        return std::make_pair(synths, groups);
    }

    template<typename functor>
    void apply_on_children(functor const & f)
    {
        for (server_node_list::iterator it = child_nodes.begin(); it != child_nodes.end(); ++it)
            f(*it);
    }

public:
    server_node * next_node(server_node * node)
    {
        assert(has_child(node));
        server_node_list::iterator next = ++server_node_list::s_iterator_to(*node);
        if (unlikely(next == child_nodes.end()))
            return 0;
        else
            return &(*next);
    }

    server_node * previous_node(server_node * node)
    {
        assert(has_child(node));
        server_node_list::iterator it = server_node_list::s_iterator_to(*node);
        if (unlikely(it == child_nodes.begin()))
            return 0;
        else
            return &(*--it);
    }

    void free_children(void)
    {
        child_nodes.clear_and_dispose(boost::mem_fn(&server_node::clear_parent));
        assert(child_synths_ == 0);
        assert(child_groups_ == 0);
    }

    void free_synths_deep(void)
    {
        child_nodes.remove_and_dispose_if(boost::mem_fn(&server_node::is_synth),
                                          boost::mem_fn(&server_node::clear_parent));

        /* now there are only group classes */
        for(server_node_list::iterator it = child_nodes.begin(); it != child_nodes.end(); ++it) {
            abstract_group * group = static_cast<abstract_group*>(&*it);
            group->free_synths_deep();
        }
        assert(child_synths_ == 0);
    }

    void remove_child(server_node * node);
    /* @} */

    void set(const char * slot_str, float val);
    void set(slot_index_t slot_id, float val);

    friend class node_graph;
    std::size_t child_synths_, child_groups_;
};


inline void server_node::clear_parent(void)
{
    if (is_synth())
        --parent_->child_synths_;
    else
        --parent_->child_groups_;

    parent_ = 0;
    release();
}

inline void server_node::set_parent(abstract_group * parent)
{
    add_ref();
    assert(parent_ == 0);
    parent_ = parent;

    if (is_synth())
        ++parent->child_synths_;
    else
        ++parent->child_groups_;
}



inline server_node * server_node::previous_node(void)
{
    return parent_->previous_node(this);
}

inline server_node * server_node::next_node(void)
{
    return parent_->next_node(this);
}


class group:
    public abstract_group
{
public:
    group(int node_id):
        abstract_group(node_id, false)
    {}

private:
    void add_child(server_node * node, node_position_constraint const & constraint);
    void add_child(server_node * node, node_position);

    void fill_queue(thread_queue & queue);

    virtual successor_container fill_queue_recursive(thread_queue & queue,
                                                     successor_container,
                                                     int activation_limit);

    friend class node_graph;
    friend class server_node;

    virtual int tail_nodes(void) const
    {
        if (empty())
            return 0;

        for (server_node_list::const_reverse_iterator it = child_nodes.rbegin(); it != child_nodes.rend(); ++it)
        {
            const server_node * tail = &*it;

            if (tail->is_synth())
                return 1;
            const abstract_group * tail_group = static_cast<const abstract_group*>(tail);
            if (!tail_group->empty())
                return tail_group->tail_nodes();
        }
        return 0;
    }
};

typedef intrusive_ptr<group> group_ptr;

class parallel_group:
    public abstract_group
{
public:
    parallel_group(int node_id):
        abstract_group(node_id, true)
    {}

private:
    void add_child(server_node * node, node_position_constraint const & constraint);
    void add_child(server_node * node, node_position);

    virtual successor_container fill_queue_recursive(thread_queue & queue,
                                                     successor_container,
                                                     int activation_limit);

    virtual int tail_nodes(void) const;

    friend class node_graph;
    friend class server_node;
};

} /* namespace nova */

#endif /* SERVER_GROUP_HPP */
