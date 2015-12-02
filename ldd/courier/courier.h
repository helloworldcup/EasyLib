
#ifndef __LDD_COURIER_COURIER_H_
#define __LDD_COURIER_COURIER_H_

#include <stdint.h>
#include <string>
#include <vector>


namespace ldd{
namespace courier{



enum CourierError{
		kCourierOk					=  0,

		kInvalidBaseParamInZk		= -202,
		kInvalidArgument			= -203,
		kNotFound					= -204,
		kInvalidHomeAddress			= -205,
		kSystemError				= -206,
		kZooKeeperNotWork			= -207
};

typedef struct {
	std::string master;
	std::vector<std::string> slaves;
	std::vector<std::string> followers;
}Roles;

enum NodeChangedType{
	kCreateNode = 0,
	kDeleteNode = 1
};



}//namespace courier
}//namespace ldd




#endif  // LDD_COURIER_RWLOCK_H_





