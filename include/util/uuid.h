#ifndef UUID_H
#define UUID_H

#include <boost/uuid.hpp>

class uuid{
public:
    uuid() = default;
    ~uuid() = default;

    static boost::uuids::uuid newone(){
        return boost::uuids::random_generator()();
    }

    static std::string newone_str(){
        return boost::uuids::to_string(newone());
    }
};

#endif