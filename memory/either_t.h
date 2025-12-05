#ifndef LITE_FNDS_EITHERT_H
#define LITE_FNDS_EITHERT_H

#include <memory>

#include "../base/inplace_base.h"

// precondition: either_state is always first or second for either_storage_base<void, U>
// empty state only exists during raw_either_storage_base default construction

namespace lite_fnds {
	template <size_t N>
	struct in_place_index {};

	using first_t = in_place_index<0>;
	using second_t = in_place_index<1>;

	constexpr static first_t to_first{};
	constexpr static second_t to_second{};

	enum class either_state {
		empty, first, second,
	};

	template <typename T, typename U,
			bool = std::is_trivially_destructible<T>::value,
			bool = std::is_trivially_destructible<U>::value>
	struct raw_either_storage_base {
		static_assert(conjunction_v<std::is_nothrow_destructible<T>, std::is_nothrow_destructible<U>>,
			"T and U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");

		union storage {
			T first;
			U second;

			constexpr storage() noexcept {}
			~storage() noexcept {}
		} _data;
        
		either_state _state;

		using first_type = T;
		using second_type = U;

		using opt = raw_inplace_storage_operations<T>;
		using opu = raw_inplace_storage_operations<U>;

		explicit raw_either_storage_base() noexcept 
			: _state {either_state::empty} {
		}

		template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::forward<Args>(args)...);
		}

		template <typename T_ = T, typename F, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#endif
			>
		raw_either_storage_base(first_t, std::initializer_list<F> il, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, std::initializer_list<F>, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), il, std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value
			) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, const T &t)
			noexcept(std::is_nothrow_copy_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), t);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, T &&t)
			noexcept(std::is_nothrow_move_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::move(t));
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept {
			if (_state == either_state::first) {
				opt::destroy_at(std::addressof(_data.first));
			} else if (_state == either_state::second) {
				opu::destroy_at(std::addressof(_data.second));
			}
		}
	};

	template <typename T, typename U>
	struct raw_either_storage_base <T, U, true, false> {
		static_assert(conjunction_v<std::is_nothrow_destructible<T>, std::is_nothrow_destructible<U>>,
			"T and U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");
		union storage {
			T first;
			U second;

			constexpr storage() noexcept {}
			~storage() noexcept {}
		} _data;
        either_state _state;

		using first_type = T;
		using second_type = U;

		using opt = raw_inplace_storage_operations<T>;
		using opu = raw_inplace_storage_operations<U>;

		explicit raw_either_storage_base() noexcept : 
			_state { either_state::empty } {
        }
		raw_either_storage_base(const raw_either_storage_base &) = default;
		raw_either_storage_base(raw_either_storage_base &&) = default;
		raw_either_storage_base &operator=(const raw_either_storage_base &) = default;
		raw_either_storage_base &operator=(raw_either_storage_base &&) = default;

		template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::forward<Args>(args)...);
		}

		template <typename T_ = T, typename F, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#endif
			>
		raw_either_storage_base(first_t, std::initializer_list<F> il, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, std::initializer_list<F>, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), il, std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, const T &t)
			noexcept(std::is_nothrow_copy_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), t);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, T &&t)
			noexcept(std::is_nothrow_move_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::move(t));
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept {
			if (_state == either_state::second) {
				opu::destroy_at(std::addressof(_data.second));
			}
		}
	};

	template <typename T, typename U>
	struct raw_either_storage_base <T, U, false, true> {
		static_assert(conjunction_v<std::is_nothrow_destructible<T>, std::is_nothrow_destructible<U>>,
			"T and U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");

		union storage {
			T first;
			U second;

			constexpr storage() noexcept {}
			~storage() noexcept {}
		} _data;
        either_state _state;

		using first_type = T;
		using second_type = U;

		using opt = raw_inplace_storage_operations<T>;
		using opu = raw_inplace_storage_operations<U>;

		explicit raw_either_storage_base() noexcept : 
			_state { either_state::empty } {
        }
		raw_either_storage_base(const raw_either_storage_base &) = default;
		raw_either_storage_base(raw_either_storage_base &&) = default;
		raw_either_storage_base &operator=(const raw_either_storage_base &) = default;
		raw_either_storage_base &operator=(raw_either_storage_base &&) = default;

				template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::forward<Args>(args)...);
		}

		template <typename T_ = T, typename F, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#endif
			>
		raw_either_storage_base(first_t, std::initializer_list<F> il, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, std::initializer_list<F>, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), il, std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value
			) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, const T &t)
			noexcept(std::is_nothrow_copy_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), t);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, T &&t)
			noexcept(std::is_nothrow_move_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::move(t));
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept {
			if (_state == either_state::first) {
				opt::destroy_at(std::addressof(_data.first));
			}
		}
	};

	template <typename T, typename U>
	struct raw_either_storage_base <T, U, true, true> {
		static_assert(conjunction_v<std::is_nothrow_destructible<T>, std::is_nothrow_destructible<U>>,
			"T and U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");

		union storage {
			T first;
			U second;

			constexpr storage() noexcept {}
		} _data;
		either_state _state;

		using first_type = T;
		using second_type = U;

		using opt = raw_inplace_storage_operations<T>;
		using opu = raw_inplace_storage_operations<U>;

		explicit raw_either_storage_base() noexcept
			: _state { either_state::empty } {
        }
        raw_either_storage_base(const raw_either_storage_base&) = default;
		raw_either_storage_base(raw_either_storage_base &&) = default;
		raw_either_storage_base &operator=(const raw_either_storage_base &) = default;
		raw_either_storage_base &operator=(raw_either_storage_base &&) = default;

		template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::forward<Args>(args)...);
		}

		template <typename T_ = T, typename F, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<F>,  Args&&...>::value>
#endif
			>
		raw_either_storage_base(first_t, std::initializer_list<F> il, Args && ... args)
			noexcept (std::is_nothrow_constructible<T_, std::initializer_list<F>, Args&&...>::value) :
			_state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), il, std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value
			) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, const T &t)
			noexcept(std::is_nothrow_copy_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), t);
		}

		template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<T_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<T_>::value>
#endif
		>
		explicit raw_either_storage_base(first_t, T &&t)
			noexcept(std::is_nothrow_move_constructible<T>::value)
			: _state{either_state::first} {
			opt::construct_at(std::addressof(_data.first), std::move(t));
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept = default;
	};

	template <typename U>
	struct raw_either_storage_base <void, U, false, true> {
		static_assert(std::is_nothrow_destructible<U>::value,
			"U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");

		union storage {
			struct {} first;
			U second;

			constexpr storage() noexcept {}
		} _data;
        either_state _state;

		using first_type = void;
		using second_type = U;
		using opu = raw_inplace_storage_operations<U>;

		raw_either_storage_base(const raw_either_storage_base &) = default;
		raw_either_storage_base(raw_either_storage_base &&) = default;
		raw_either_storage_base &operator=(const raw_either_storage_base &) = default;
		raw_either_storage_base &operator=(raw_either_storage_base &&) = default;

		explicit raw_either_storage_base() noexcept : 
			_state{ either_state::empty } { }
		
		explicit raw_either_storage_base(first_t) noexcept : 
			_state{ either_state::first } { }

		template <typename U_ = U, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
	typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
	typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value
			) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}
		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept = default;
	};

	template <typename U>
	struct raw_either_storage_base <void, U, false, false> {
		static_assert(std::is_nothrow_destructible<U>::value,
			"U must be nothrow destructible");
		static_assert(can_strong_move_or_copy_constructible<U>::value,
			"The second type U must be nothrow copy constructible or be nothrow move constructible.");

		union storage {
			struct {} first;
			U second;

			constexpr storage() noexcept {}
			~storage() noexcept {}
		} _data;
		either_state _state;

		using first_type = void;
		using second_type = U;
		using opu = raw_inplace_storage_operations<U>;
		
		explicit raw_either_storage_base() noexcept : 
			_state{ either_state::empty } { }
		
		explicit raw_either_storage_base(first_t) noexcept : 
			_state{ either_state::first } {}

		template <typename U_ = U, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, Args &&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, Args &&...>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, Args &&...>::value) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::forward<Args>(args)...);
		}

		template <typename U_ = U, typename F, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_constructible<U_, std::initializer_list<F>,  Args&&...>::value>
#endif
		>
		raw_either_storage_base(second_t, std::initializer_list<F> il, Args &&... args)
			noexcept (std::is_nothrow_constructible<U_, std::initializer_list<F>, Args &&...>::value
			) : _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), il, std::forward<Args>(args)...);
		}
		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_copy_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, const U &u)
			noexcept(std::is_nothrow_copy_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), u);
		}

		template <typename U_ = U,
#if LFNDS_HAS_EXCEPTIONS
			typename = std::enable_if_t<std::is_move_constructible<U_>::value>
#else
			typename = std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>
#endif
		>
		explicit raw_either_storage_base(second_t, U &&u)
			noexcept(std::is_nothrow_move_constructible<U>::value)
			: _state{either_state::second} {
			opu::construct_at(std::addressof(_data.second), std::move(u));
		}

		~raw_either_storage_base() noexcept {
			if (_state == either_state::second) {
				opu::destroy_at(std::addressof(_data.second));
			}
		}
	};

	template <typename T, typename U>
	struct either_storage_base : raw_either_storage_base<T, U> {
		using base = raw_either_storage_base<T, U>;
		using base::base;
		using opt = typename base::opt;
		using opu = typename base::opu;
#if LFNDS_HAS_EXCEPTIONS
		template <typename T_ = T, typename U_ = U, typename ... Args,
			std::enable_if_t<conjunction_v<std::is_constructible<T_, Args&&...>,
				std::is_nothrow_move_constructible<U_>>>* = nullptr>
		void emplace_first(Args &&... args)
			noexcept(conjunction_v<
					std::integral_constant<bool,
						noexcept(opt::emplace_at(static_cast<T_*>(nullptr), std::forward<Args>(args)...))>,
					std::is_nothrow_constructible<T_, Args&&...>>) {
			if (this->has_first()) {
				opt::emplace_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
				return;
			}

			U backup(std::move(this->get_second()));
			opu::destroy_at(std::addressof(this->_data.second));
#if LFNDS_COMPILER_HAS_EXCEPTIONS
			try {
#endif
				opt::construct_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
				this->_state = either_state::first;
#if LFNDS_COMPILER_HAS_EXCEPTIONS
			} catch (...) {
				opu::construct_at(std::addressof(this->_data.second), std::move(backup));
				throw;
			}
#endif
		}

		template <typename T_ = T, typename U_ = U, typename... Args,
			std::enable_if_t<conjunction_v<std::is_constructible<T_, Args &&...>,
				negation<std::is_nothrow_move_constructible<U_>> > >* = nullptr>
		void emplace_first(Args &&... args)
			noexcept(conjunction_v<
				std::integral_constant<bool, noexcept(opt::emplace_at(static_cast<T_*>(nullptr),
				                                                      std::forward<Args>(args)...))>,
				std::is_nothrow_constructible<T_, Args &&...> >) {
			if (this->has_first()) {
				opt::emplace_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
				return;
			}

			U backup(this->get_second());
			opu::destroy_at(std::addressof(this->_data.second));
#if LFNDS_COMPILER_HAS_EXCEPTIONS
			try {
#endif
				opt::construct_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
				this->_state = either_state::first;
#if LFNDS_COMPILER_HAS_EXCEPTIONS
			} catch (...) {
				opu::construct_at(std::addressof(this->_data.second), backup);
				throw;
			}
#endif
		}

		template <typename T_ = T, typename U_ = U, typename... Args,
			std::enable_if_t<conjunction_v<std::is_constructible<U_, Args&&...>,
				std::is_nothrow_move_constructible<U_>>>* = nullptr>
		void emplace_second(Args &&... args)
			noexcept(conjunction_v<std::is_nothrow_destructible<T_>, std::is_nothrow_constructible<U_, Args&&...>,
			         std::integral_constant<bool,
						noexcept(opu::emplace_at(static_cast<U_*>(nullptr), std::forward<Args>(args)...))>>) {
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
				return;
			}

			U_ tmp(std::forward<Args>(args)...);
			opt::destroy_at(std::addressof(this->_data.first));
			opu::construct_at(std::addressof(this->_data.second), std::move(tmp));
			this->_state = either_state::second;
		}

		template <typename T_ = T, typename U_ = U, typename... Args,
			std::enable_if_t<conjunction_v<std::is_constructible<U_, Args&&...>,
				negation<std::is_nothrow_move_constructible<U_>>>>* = nullptr>
		void emplace_second(Args &&... args)
			noexcept(conjunction_v<std::is_nothrow_destructible<T_>, std::is_nothrow_constructible<U_, Args&&...>,
				 std::integral_constant<bool, noexcept(opu::emplace_at(static_cast<U_*>(nullptr), std::forward<Args>(args)...))>>) {
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
				return;
			}

			U tmp(std::forward<Args>(args)...);
			opt::destroy_at(std::addressof(this->_data.first));
			opu::construct_at(std::addressof(this->_data.second), tmp);
			this->_state = either_state::second;
		}
#else
		template <typename T_ = T, typename ... Args,
			std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
		void emplace_first(Args &&... args) noexcept {
			if (this->has_first()) {
				opt::emplace_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
				return;
			}

			opu::destroy_at(std::addressof(this->_data.second));
			opt::construct_at(std::addressof(this->_data.first), std::forward<Args>(args)...);
			this->_state = either_state::first;
		}

		template <typename U_ = U, typename... Args,
			std::enable_if_t<std::is_nothrow_constructible<U_, Args&&...>::value>* = nullptr>
		void emplace_second(Args &&... args) noexcept {
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
				return;
			}

			opt::destroy_at(std::addressof(this->_data.first));
			opu::construct_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
			this->_state = either_state::second;
		}
#endif

		template <typename T_, typename U_,
			std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, T_&&>,	std::is_constructible<U, U_&&>
#else
				std::is_nothrow_constructible<T, T_&&>,	std::is_nothrow_constructible<U, U_&&>
#endif
		>>* = nullptr>
		void assign(either_storage_base<T_, U_>&& rhs)
			noexcept(conjunction_v<
				std::is_nothrow_constructible<T, T_&&>, std::is_nothrow_constructible<U, U_&&>,
				std::integral_constant<bool,
					noexcept(std::declval<either_storage_base&>().emplace_first(std::declval<T&&>()))>,
				std::integral_constant<bool,
					noexcept(std::declval<either_storage_base&>().emplace_second(std::declval<U&&>()))>>) {
            if (static_cast<const void*>(this) == static_cast<const void*>(std::addressof(rhs))) {
                return;
            }

			if (rhs.has_first()) {
				this->emplace_first(std::move(rhs).get_first());
			} else {
				this->emplace_second(std::move(rhs).get_second());
			}
		}

		template <typename T_, typename U_,
			std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, const T_&>, std::is_constructible<U, const U_&>
#else
				std::is_nothrow_constructible<T, const T_&>, std::is_nothrow_constructible<U, const U_&>
#endif
		>>* = nullptr>
		void assign(const either_storage_base<T_, U_> &rhs)
			noexcept(conjunction_v<
				std::is_nothrow_constructible<T, const T_&>, std::is_nothrow_constructible<U, const U_&>,
				disjunction<std::is_nothrow_copy_constructible<T_>, std::is_nothrow_copy_assignable<T_>>,
				disjunction<std::is_nothrow_copy_constructible<U_>, std::is_nothrow_copy_assignable<U_>>>) {
            if (static_cast<const void*>(this) == static_cast<const void*>(std::addressof(rhs))) {
                return;
            }

			if (rhs.has_first()) {
				this->emplace_first(rhs.get_first());
			} else {
				this->emplace_second(rhs.get_second());
			}
		}

		constexpr bool has_first() const noexcept {
			return this->_state == either_state::first;
		}

		constexpr T& get_first() & noexcept {
			return this->_data.first;
		}

		constexpr const T& get_first() const& noexcept {
			return this->_data.first;
		}

		constexpr T&& get_first() && noexcept {
			return std::move(this->_data.first);
		}

		constexpr U& get_second() & noexcept {
			return this->_data.second;
		}

		constexpr const U& get_second() const& noexcept {
			return this->_data.second;
		}

		constexpr U&& get_second() && noexcept {
			return std::move(this->_data.second);
		}
	};

	template <typename U>
	struct either_storage_base <void, U> : raw_either_storage_base<void, U> {
		using base = raw_either_storage_base<void, U>;
		using base::base;
		using opu = typename base::opu;

		void emplace_first() {
			if (this->has_first()) {
				return;
			}
			opu::destroy_at(std::addressof(this->_data.second));
			this->_state = either_state::first;
		}

		template <typename U_ = U, typename ... Args,
			std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U_, Args&&...>::value
#else
				std::is_nothrow_constructible<U_, Args&&...>::value
#endif
		>* = nullptr>
		void emplace_second(Args &&... args)
			noexcept(noexcept(opu::emplace_at(static_cast<U_*>(nullptr), std::forward<Args>(args)...))) {
			if (this->has_first()) {
				opu::construct_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
				this->_state = either_state::second;
				return;
			}
			opu::emplace_at(std::addressof(this->_data.second), std::forward<Args>(args)...);
		}

		template <typename U_, std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
			std::is_constructible<U_, U_&&>::value
#else
			std::is_nothrow_constructible<U_, U_&&>::value
#endif
		>* = nullptr>
		void assign(either_storage_base<void, U_>&& rhs)
			noexcept(conjunction_v<std::is_nothrow_constructible<U, U_&&>,
				std::integral_constant<bool,
					noexcept(std::declval<either_storage_base&>().emplace_second(std::declval<U&&>()))>>) {
			if (reinterpret_cast<const void*>(this) == reinterpret_cast<const void*>(&rhs)) {
				return;
			}

			if (rhs.has_first()) {
				this->emplace_first();
			} else {
				this->emplace_second(std::move(rhs).get_second());
			}
		}

		template <typename U_,
			std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U_, const U_&>::value
#else
				std::is_nothrow_constructible<U_, const U_&>::value
#endif
		>* = nullptr>
		void assign(const either_storage_base<void, U_> &rhs)
			noexcept(conjunction_v<std::is_nothrow_constructible<U, const U_&>,
				disjunction<std::is_nothrow_copy_constructible<U_>, std::is_nothrow_copy_assignable<U_>>>) {
			if (reinterpret_cast<const void*>(this) == reinterpret_cast<const void*>(&rhs)) {
				return;
			}

			if (rhs.has_first()) {
				this->emplace_first();
			} else {
				this->emplace_second(rhs.get_second());
			}
		}

		constexpr bool has_first() const noexcept {
			return this->_state == either_state::first;
		}

		constexpr U& get_second() & noexcept {
			return this->_data.second;
		}

		constexpr const U& get_second() const& noexcept {
			return this->_data.second;
		}

		constexpr U&& get_second() && noexcept {
			return std::move(this->_data.second);
		}
	};

	// copy construct
    template <typename T, typename U,
		bool = conjunction_v<disjunction<std::is_void<T>,
			std::is_copy_constructible<T>>, std::is_copy_constructible<U>>,
		bool = conjunction_v<disjunction<std::is_void<T>,
			std::is_trivially_copy_constructible<T>>, std::is_trivially_copy_constructible<U>>>
    struct either_storage_copy_construct_base : either_storage_base<T, U> {
        using either_storage_base<T, U>::either_storage_base;
    };

	template <typename T, typename U>
	struct either_storage_copy_construct_base <T, U, true, false>: either_storage_base<T, U> {
        using either_storage_base<T, U>::either_storage_base;

        either_storage_copy_construct_base() = default;
        either_storage_copy_construct_base(const either_storage_copy_construct_base& rhs)
			noexcept(conjunction_v<std::is_nothrow_copy_constructible<T>, std::is_nothrow_copy_constructible<U>>)  {
        	using opt = typename either_storage_base<T, U>::opt;
        	using opu = typename either_storage_base<T, U>::opu;

        	if (rhs.has_first()) {
        		opt::construct_at(std::addressof(this->_data.first), rhs.get_first());
        	} else {
        		opu::construct_at(std::addressof(this->_data.second), rhs.get_second());
        	}
        	this->_state = rhs._state;
        }
        either_storage_copy_construct_base(either_storage_copy_construct_base &&rhs) = default;
        either_storage_copy_construct_base &operator=(const either_storage_copy_construct_base &rhs) = default;
        either_storage_copy_construct_base &operator=(either_storage_copy_construct_base &&rhs) = default;
    };

	template <typename U>
	struct either_storage_copy_construct_base <void, U, true, false>: either_storage_base<void, U> {
		using either_storage_base<void, U>::either_storage_base;

		either_storage_copy_construct_base() = default;
		either_storage_copy_construct_base(const either_storage_copy_construct_base& rhs)
		noexcept(conjunction_v<std::is_nothrow_copy_constructible<U>>)  {
			using opu = typename either_storage_base<void, U>::opu;
			if (!rhs.has_first()) {
				opu::construct_at(std::addressof(this->_data.second), rhs.get_second());
			}
			this->_state = rhs._state;
		}
		either_storage_copy_construct_base(either_storage_copy_construct_base &&rhs) = default;
		either_storage_copy_construct_base &operator=(const either_storage_copy_construct_base &rhs) = default;
		either_storage_copy_construct_base &operator=(either_storage_copy_construct_base &&rhs) = default;
	};

    // move construct
    template <typename T, typename U,
		bool = conjunction_v<disjunction<std::is_void<T>,
			std::is_move_constructible<T>>, std::is_move_constructible<U>>,
		bool = conjunction_v<disjunction<std::is_void<T>,
			std::is_trivially_move_constructible<T>>, std::is_trivially_move_constructible<U>>>
    struct either_storage_move_construct_base : either_storage_copy_construct_base <T, U> {
        using either_storage_copy_construct_base<T, U>::either_storage_copy_construct_base;
    };

	template <typename T, typename U>
	struct either_storage_move_construct_base <T, U, true, false> : either_storage_copy_construct_base <T, U> {
		using either_storage_copy_construct_base<T, U>::either_storage_copy_construct_base;

        either_storage_move_construct_base() = default;
        either_storage_move_construct_base(const either_storage_move_construct_base &rhs) = default;

        either_storage_move_construct_base(either_storage_move_construct_base &&rhs)
			noexcept(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_move_constructible<U>>) {
        	using opt = typename either_storage_copy_construct_base<T, U>::opt;
        	using opu = typename either_storage_copy_construct_base<T, U>::opu;

        	if (rhs.has_first()) {
        		opt::construct_at(std::addressof(this->_data.first), std::move(rhs).get_first());
        	} else {
        		opu::construct_at(std::addressof(this->_data.second), std::move(rhs).get_second());
        	}
        	this->_state = rhs._state;
        }

        either_storage_move_construct_base &operator=(const either_storage_move_construct_base &rhs) = default;
        either_storage_move_construct_base &operator=(either_storage_move_construct_base &&rhs) = default;
    };

	template <typename U>
	struct either_storage_move_construct_base <void, U, true, false> : either_storage_copy_construct_base <void, U> {
		using either_storage_copy_construct_base<void, U>::either_storage_copy_construct_base;

		either_storage_move_construct_base() = default;
		either_storage_move_construct_base(const either_storage_move_construct_base &rhs) = default;

		either_storage_move_construct_base(either_storage_move_construct_base &&rhs)
			noexcept(std::is_nothrow_move_constructible<U>::value) {
			using opu = typename either_storage_move_construct_base<void, U>::opu;

			if (!rhs.has_first()) {
				opu::construct_at(std::addressof(this->_data.second), std::move(rhs).get_second());
			}
			this->_state = rhs._state;
		}

		either_storage_move_construct_base &operator=(const either_storage_move_construct_base &rhs) = default;
		either_storage_move_construct_base &operator=(either_storage_move_construct_base &&rhs) = default;
	};

    // copy assign policy
	template <typename T, typename U,
		bool = disjunction_v<std::is_void<T>, can_strong_replace<T>>,
		bool = conjunction_v<std::is_trivially_copy_assignable<T>, std::is_trivially_copy_assignable<U>>>
	struct either_storage_copy_assign_base : either_storage_move_construct_base <T, U> {
        using either_storage_move_construct_base<T, U>::either_storage_move_construct_base;
    };

	template <typename T, typename U>
    struct either_storage_copy_assign_base <T, U, true, false> : either_storage_move_construct_base <T, U> {
        using either_storage_move_construct_base<T, U>::either_storage_move_construct_base;
        either_storage_copy_assign_base() = default;
        either_storage_copy_assign_base(const either_storage_copy_assign_base &rhs) = default;
        either_storage_copy_assign_base(either_storage_copy_assign_base &&rhs) = default;
        either_storage_copy_assign_base &operator=(const either_storage_copy_assign_base &rhs)
            noexcept(noexcept(std::declval<either_storage_base<T, U>&>().assign(
            		std::declval<const either_storage_base<T,U>&>()))) {
        	this->assign(rhs);
            return *this;
        }
        either_storage_copy_assign_base &operator=(either_storage_copy_assign_base &&rhs) = default;
    };

    // move assign policy
    template <typename T, typename U,
		bool = disjunction_v<std::is_void<T>, can_strong_replace<T>>,
	    bool = conjunction_v<std::is_trivially_move_assignable<T>, std::is_trivially_move_assignable<U>> >
    struct either_storage_move_assign_base : either_storage_copy_assign_base<T, U> {
	    using either_storage_copy_assign_base<T, U>::either_storage_copy_assign_base;
    };

    template <typename T, typename U>
    struct either_storage_move_assign_base<T, U, true, false> : either_storage_copy_assign_base<T, U> {
	    using either_storage_copy_assign_base<T, U>::either_storage_copy_assign_base;

	    either_storage_move_assign_base() = default;
	    either_storage_move_assign_base(const either_storage_move_assign_base &rhs) = default;
	    either_storage_move_assign_base(either_storage_move_assign_base &&rhs) = default;
	    either_storage_move_assign_base &operator=(const either_storage_move_assign_base &rhs) = default;
    	either_storage_move_assign_base &operator=(either_storage_move_assign_base &&rhs)
			noexcept(noexcept(std::declval<either_storage_base<T, U>&>().assign(
					std::declval<either_storage_base<T,U>&&>()))) {
    		this->assign(std::move(rhs));
    		return *this;
    	}
    };

	template <typename T, typename U, bool EnableCopy, bool EnableMove>
    struct either_ctor_delete_base;

    template <typename T, typename U>
    struct either_ctor_delete_base <T, U, true, true> {
        either_ctor_delete_base() = default;
        either_ctor_delete_base(const either_ctor_delete_base&) = default;
        either_ctor_delete_base(either_ctor_delete_base&&) noexcept = default;
        either_ctor_delete_base& operator=(const either_ctor_delete_base&) = default;
        either_ctor_delete_base& operator=(either_ctor_delete_base&&) noexcept = default;
    };

    template <typename T, typename U>
    struct either_ctor_delete_base<T, U, true, false> {
        either_ctor_delete_base() = default;
        either_ctor_delete_base(const either_ctor_delete_base&) = default;
        either_ctor_delete_base(either_ctor_delete_base&&) = delete;
        either_ctor_delete_base& operator=(const either_ctor_delete_base&) = default;
        either_ctor_delete_base& operator=(either_ctor_delete_base&&) noexcept = default;
    };

    template <typename T, typename U>
    struct either_ctor_delete_base<T, U, false, true> {
        either_ctor_delete_base() = default;
        either_ctor_delete_base(const either_ctor_delete_base&) = delete;
        either_ctor_delete_base(either_ctor_delete_base&&) noexcept = default;
        either_ctor_delete_base& operator=(const either_ctor_delete_base&) = default;
        either_ctor_delete_base& operator=(either_ctor_delete_base&&) noexcept = default;
    };

    template <typename T, typename U>
    struct either_ctor_delete_base<T, U, false, false> {
        either_ctor_delete_base() = default;
        either_ctor_delete_base(const either_ctor_delete_base&) = delete;
        either_ctor_delete_base(either_ctor_delete_base&&) noexcept = delete;
        either_ctor_delete_base& operator=(const either_ctor_delete_base&) = default;
        either_ctor_delete_base& operator=(either_ctor_delete_base&&) noexcept = default;
    };

    // assign deleter
    template <typename T, typename U, bool EnableCopy, bool EnableMove>
    struct either_assign_delete_base;

	template <typename T, typename U>
    struct either_assign_delete_base <T, U, true, true> {
        either_assign_delete_base() = default;
        either_assign_delete_base(const either_assign_delete_base&) = default;
        either_assign_delete_base(either_assign_delete_base&&) noexcept = default;
        either_assign_delete_base& operator=(const either_assign_delete_base&) = default;
        either_assign_delete_base& operator=(either_assign_delete_base&&) noexcept = default;
    };

	template <typename T, typename U>
    struct either_assign_delete_base<T, U, true, false> {
        either_assign_delete_base() = default;
        either_assign_delete_base(const either_assign_delete_base&) = default;
        either_assign_delete_base(either_assign_delete_base&&) noexcept = default;
        either_assign_delete_base& operator=(const either_assign_delete_base&) = default;
        either_assign_delete_base& operator=(either_assign_delete_base&&) noexcept = delete;
    };

	template <typename T, typename U>
    struct either_assign_delete_base<T, U, false, true> {
        either_assign_delete_base() = default;
        either_assign_delete_base(const either_assign_delete_base&) = default;
        either_assign_delete_base(either_assign_delete_base&&) noexcept = default;
        either_assign_delete_base& operator=(const either_assign_delete_base&) = delete;
        either_assign_delete_base& operator=(either_assign_delete_base&&) noexcept = default;
    };

	template <typename T, typename U>
    struct either_assign_delete_base<T, U, false, false> {
        either_assign_delete_base() = default;
        either_assign_delete_base(const either_assign_delete_base&) = default;
        either_assign_delete_base(either_assign_delete_base&&) noexcept = default;
        either_assign_delete_base& operator=(const either_assign_delete_base&) = delete;
        either_assign_delete_base& operator=(either_assign_delete_base&&) noexcept = delete;
    };

	template <typename T, typename U>
	struct either_t :
		private either_storage_move_assign_base<T, U>,
		private either_ctor_delete_base<T, U,
#if LFNDS_HAS_EXCEPTIONS
			conjunction_v<disjunction<std::is_void<T>, std::is_copy_constructible<T>>, std::is_copy_constructible<U> >,
			conjunction_v<disjunction<std::is_void<T>, std::is_move_constructible<T>>, std::is_move_constructible<U> >
#else
			conjunction_v<disjunction<std::is_void<T>, std::is_nothrow_copy_constructible<T>>, std::is_nothrow_copy_constructible<U> >,
			conjunction_v<disjunction<std::is_void<T>, std::is_nothrow_move_constructible<T>>, std::is_nothrow_move_constructible<U> >
#endif
		>,
		private either_assign_delete_base<T, U,
#if LFNDS_HAS_EXCEPTIONS
			conjunction_v<disjunction<std::is_void<T>, std::is_copy_assignable<T>>, std::is_copy_assignable<U> >,
			conjunction_v<disjunction<std::is_void<T>, std::is_move_assignable<T>>, std::is_move_assignable<U> >
#else
			conjunction_v<disjunction<std::is_void<T>, std::is_nothrow_copy_assignable<T>>, std::is_nothrow_copy_assignable<U> >,
			conjunction_v<disjunction<std::is_void<T>, std::is_nothrow_move_assignable<T>>, std::is_nothrow_move_assignable<U> >
#endif
		> {
	private:
		using base = either_storage_move_assign_base<T, U>;
	public:
		using base::base;
		using first_type = typename base::first_type;
		using second_type = typename base::second_type;
		using base::has_first;
		using base::emplace_first;
		using base::emplace_second;

		either_t() = delete;
		either_t(const either_t &) = default;
		either_t(either_t &&) noexcept = default;
		either_t &operator=(const either_t &) = default;
		either_t &operator=(either_t &&) noexcept = default;
		~either_t() = default;

		template <typename T_, typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_void<T_>>,
				negation<conjunction<std::is_same<T, T_>, std::is_same<U, U_>>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, T_&&>, std::is_constructible<U, U_&&>
#else
				std::is_nothrow_constructible<T, T_&&>, std::is_nothrow_constructible<U, U_&&>
#endif
		>>>
		explicit either_t(either_t<T_, U_>&& rhs) noexcept(
			conjunction_v<std::is_nothrow_constructible<T, T_&&>,
				std::is_nothrow_constructible<U, U_&&>>) {
			using opt = typename base::opt;
			using opu = typename base::opu;

			if (rhs.has_first()) {
				opt::construct_at(std::addressof(static_cast<base*>(this)->_data.first),
					std::move(rhs.get_first()));
                static_cast<base*>(this)->_state = either_state::first;
			} else {
				opu::construct_at(std::addressof(static_cast<base*>(this)->_data.second),
					std::move(rhs.get_second()));
                static_cast<base*>(this)->_state = either_state::second;
			}
		}

		template <typename T_, typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_void<T_>>,
				negation<conjunction<std::is_same<T, T_>, std::is_same<U, U_>>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, const T_&>, std::is_constructible<U, const U_&>
#else
				std::is_nothrow_constructible<T, const T_&>, std::is_nothrow_constructible<U, const U_&>
#endif
		>>>
		explicit either_t(const either_t<T_, U_>& rhs) noexcept(
			conjunction_v<std::is_nothrow_constructible<T, const T_&>,
				std::is_nothrow_constructible<U, const U_&>>) {
			using opt = typename base::opt;
			using opu = typename base::opu;
			if (rhs.has_first()) {
				opt::construct_at(std::addressof(static_cast<base*>(this)->_data.first), rhs.get_first());
                static_cast<base*>(this)->_state = either_state::first;
			} else {
				opu::construct_at(std::addressof(static_cast<base*>(this)->_data.second), rhs.get_second());
                static_cast<base*>(this)->_state = either_state::second;
			}
		}

		template <typename T_, typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_void<T_>>, negation<std::is_void<U_>>,
				negation<std::is_same<T_, T> >, negation<std::is_same<U_, U> >,
				can_strong_replace<T_>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, T_&&>, std::is_constructible<U, U_&&>
#else
				std::is_nothrow_constructible<T, T_&&>, std::is_nothrow_constructible<U, U_&&>
#endif
		>>>
		either_t& operator=(either_t<T_, U_>&& rhs)
			noexcept(noexcept(std::declval<base&>().assign(std::move(rhs)))) {
			this->assign(std::move(rhs));
			return *this;
		}

		template <typename T_, typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_void<T_>>, negation<std::is_void<U_>>,
				negation<std::is_same<T_, T>>, negation<std::is_same<U_, U>>,
				can_strong_replace<T_>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<T, const T_&>, std::is_constructible<U, const U_&>
#else
				std::is_nothrow_constructible<T, const T_&>, std::is_nothrow_constructible<U, const U_&>
#endif
		>>>
		either_t& operator=(const either_t<T_, U_>& rhs)
			noexcept(noexcept(std::declval<base&>().assign(rhs))) {
			this->assign(rhs);
			return *this;
		}

		T& get_first() & noexcept {
			return static_cast<base&>(*this).get_first();
		}

		const T& get_first() const & noexcept {
			return static_cast<const base&>(*this).get_first();
		}

        T&& get_first() && noexcept {
            return static_cast<base&&>(*this).get_first();
        }

		U& get_second() & noexcept {
            return static_cast<base&>(*this).get_second();
        }

		const U& get_second() const & noexcept {
			return static_cast<const base&>(*this).get_second();
		}
		
		U&& get_second() && noexcept {
            return static_cast<base&&>(*this).get_second();
		}

		template <typename T_ = T,
			std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_move_constructible<T_>, can_strong_replace<T_>
#else
				std::is_nothrow_move_constructible<T_>
#endif
		>>* = nullptr>
		either_t& operator=(std::add_rvalue_reference_t<std::decay_t<T_>> t)
			noexcept(noexcept(std::declval<either_t&>().emplace_first(std::declval<std::decay_t<T_>&&>()))) {
			this->emplace_first(std::move(t));
			return *this;
		}

		template <typename T_ = T,
			std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_copy_constructible<T_>, can_strong_replace<T_>
#else
				std::is_nothrow_copy_constructible<T_>
#endif
		>>* = nullptr>
		either_t& operator=(std::add_lvalue_reference_t<std::decay_t<const T_>> t)
			noexcept(noexcept(std::declval<either_t&>().emplace_first(std::declval<const std::decay_t<T_>&>()))) {
			this->emplace_first(t);
			return *this;
		}

		template <typename U_ = U, std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>* = nullptr>
		either_t& operator=(std::add_rvalue_reference_t<std::decay_t<U_>> u) noexcept {
			using opt = typename base::opt;
			using opu = typename base::opu;
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::move(u));
			} else {
				opt::destroy_at(std::addressof(this->_data.first));
				opu::construct_at(std::addressof(this->_data.second), std::move(u));
				this->_state = either_state::second;
			}
			return *this;
		}

#if !LFNDS_HAS_EXCEPTIONS
		template <typename U_ = U, std::enable_if_t<conjunction_v<std::is_move_constructible<U_>,
			negation<std::is_nothrow_move_constructible<U_>>>>* = nullptr>
		either_t& operator=(std::add_rvalue_reference_t<std::decay_t<U_>> u) {
			using opt = typename base::opt;
			using opu = typename base::opu;
			U tmp(std::move(u));
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), tmp);
			} else {
				opt::destroy_at(std::addressof(this->_data.first));
				opu::construct_at(std::addressof(this->_data.second), tmp);
				this->_state = either_state::second;
			}
			return *this;
		}
#endif

		template <typename U_ = U, std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>* = nullptr>
		either_t& operator=(std::add_lvalue_reference_t<std::decay_t<const U_>> u) noexcept {
			using opt = typename base::opt;
			using opu = typename base::opu;
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), u);
			} else {
				opt::destroy_at(std::addressof(this->_data.first));
				opu::construct_at(std::addressof(this->_data.second), u);
				this->_state = either_state::second;
			}
			return *this;
		}

#if !LFNDS_HAS_EXCEPTIONS
		template <typename U_ = U, std::enable_if_t<conjunction_v<
			std::is_copy_constructible<U_>,
			negation<std::is_nothrow_copy_constructible<U_>>>>* = nullptr>
		either_t& operator=(std::add_lvalue_reference_t<std::decay_t<const U_>> u) {
			using opt = typename base::opt;
			using opu = typename base::opu;
			U tmp(u);
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::move(tmp));
			} else {
				opt::destroy_at(std::addressof(this->_data.first));
				opu::construct_at(std::addressof(this->_data.second), std::move(tmp));
				this->_state = either_state::second;
			}
			return *this;
		}
#endif

		void swap_both_first(either_t& rhs)
			noexcept(is_nothrow_swappable<T>::value) {
			using std::swap;
			swap(this->get_first(), rhs.get_first());
		}

		using t_is_nothrow_move_constructible = std::true_type;
		using t_is_nothrow_copy_constructible = std::false_type;
		using u_is_nothrow_move_constructible = std::true_type;
		using u_is_nothrow_copy_constructible = std::false_type;

		void swap_one_first_another_second_impl(either_t& rhs,
				t_is_nothrow_move_constructible,
				u_is_nothrow_move_constructible) noexcept {
			T tmp = std::move(this->get_first());

			base::opt::destroy_at(std::addressof(this->_data.first));
			base::opu::construct_at(std::addressof(this->_data.second), std::move(rhs._data.second));
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			base::opt::construct_at(std::addressof(rhs._data.first), std::move(tmp));
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second_impl(either_t& rhs,
				t_is_nothrow_move_constructible,
				u_is_nothrow_copy_constructible) noexcept {
			T tmp = std::move(this->get_first());

			base::opt::destroy_at(std::addressof(this->_data.first));
			base::opu::construct_at(std::addressof(this->_data.second), rhs._data.second);
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			base::opt::construct_at(std::addressof(rhs._data.first), std::move(tmp));
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second_impl(either_t& rhs,
				t_is_nothrow_copy_constructible,
				u_is_nothrow_move_constructible) noexcept {
			T tmp = this->get_first();

			base::opt::destroy_at(std::addressof(this->_data.first));
			base::opu::construct_at(std::addressof(this->_data.second), std::move(rhs._data.second));
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			base::opt::construct_at(std::addressof(rhs._data.first), tmp);
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second_impl(either_t& rhs,
			t_is_nothrow_copy_constructible,
			u_is_nothrow_copy_constructible) noexcept {
			T tmp = this->get_first();

			base::opt::destroy_at(std::addressof(this->_data.first));
			base::opu::construct_at(std::addressof(this->_data.second), rhs._data.second);
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			base::opt::construct_at(std::addressof(rhs._data.first), tmp);
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second(either_t& rhs) noexcept {
			using std::swap;
			swap_one_first_another_second_impl(rhs,
				std::is_nothrow_move_constructible<T>(),
				std::is_nothrow_move_constructible<U>());
		}

		template <typename T_ = T, typename U_ = U,
			std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
				conjunction_v<is_swappable<T_>, is_swappable<U_>, can_strong_move_or_copy_constructible<T_>>
#else
				conjunction_v<is_nothrow_swappable<T>, is_nothrow_swappable<U>>
#endif
		>* = nullptr>
		void swap(either_t& rhs)
			noexcept(conjunction_v<is_nothrow_swappable<T>, is_nothrow_swappable<U>>) {
			if (this == std::addressof(rhs)) {
				return;
			}

			if (this->has_first() && rhs.has_first()) {
				swap_both_first(rhs);
			} else if (this->has_first() && !rhs.has_first()) {
				swap_one_first_another_second(rhs);
			} else if (!this->has_first() && rhs.has_first()) {
				rhs.swap(*this);
			} else {
				using std::swap;
				swap(this->get_second(), rhs.get_second());
			}
		}
	};

	template <typename U>
	struct either_t <void, U> :
		private either_storage_move_assign_base<void, U>,
		private either_ctor_delete_base<void, U,
#if LFNDS_HAS_EXCEPTIONS
			std::is_copy_constructible<U>::value,
			std::is_move_constructible<U>::value
#else
			std::is_nothrow_copy_constructible<U>::value,
			std::is_nothrow_move_constructible<U>::value
#endif
		>,
		private either_assign_delete_base<void, U,
#if LFNDS_HAS_EXCEPTIONS
			std::is_copy_assignable<U>::value,
			std::is_move_assignable<U>::value
#else
			std::is_nothrow_copy_assignable<U>::value,
			std::is_nothrow_move_assignable<U>::value
#endif
		> {
	private:
		using base = either_storage_move_assign_base<void, U>;
	public:
		using base::base;
		using first_type = typename base::first_type;
		using second_type = typename base::second_type;
		using base::has_first;
		using base::emplace_first;
		using base::emplace_second;

		either_t() = delete;
		either_t(const either_t &) = default;
		either_t(either_t &&) noexcept = default;
		either_t &operator=(const either_t &) = default;
		either_t &operator=(either_t &&) noexcept = default;
		~either_t() = default;

		template <typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_same<U, U_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U, U_&&>
#else
				std::is_nothrow_constructible<U, U_&&>
#endif
		>>>
		explicit either_t(either_t<void, U_>&& rhs)
			noexcept(std::is_nothrow_constructible<U, U_&&>::value) {
			using opu = typename base::opu;

			if (rhs.has_first()) {
                static_cast<base*>(this)->_state = either_state::first;
			} else {
				opu::construct_at(std::addressof(static_cast<base*>(this)->_data.second),
					std::move(rhs.get_second()));
                static_cast<base*>(this)->_state = either_state::second;
			}
		}

		template <typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_same<U, U_>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U, const U_&>
#else
				std::is_nothrow_constructible<U, const U_&>
#endif
		>>>
		explicit either_t(const either_t<void, U_>& rhs)
			noexcept(std::is_nothrow_constructible<U, const U_&>::value) {
			using opu = typename base::opu;
			if (rhs.has_first()) {
                static_cast<base*>(this)->_state = either_state::first;
			} else {
				opu::construct_at(std::addressof(static_cast<base*>(this)->_data.second), rhs.get_second());
                static_cast<base*>(this)->_state = either_state::second;
			}
		}

		template <typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_same<U_, U>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U, U_&&>
#else
				std::is_nothrow_constructible<U, U_&&>
#endif
		>>>
		either_t& operator=(either_t<void, U_>&& rhs)
			noexcept(noexcept(std::declval<base&>().assign(std::move(rhs)))) {
			this->assign(std::move(rhs));
			return *this;
		}

		template <typename U_,
			typename = std::enable_if_t<conjunction_v<
				negation<std::is_same<U_, U>>,
#if LFNDS_HAS_EXCEPTIONS
				std::is_constructible<U, const U_&>
#else
				std::is_nothrow_constructible<U, const U_&>
#endif
		>>>
		either_t& operator=(const either_t<void, U_>& rhs)
			noexcept(noexcept(std::declval<base&>().assign(rhs))) {
			this->assign(rhs);
			return *this;
		}

		U& get_second() & noexcept {
            return static_cast<base&>(*this).get_second();
        }

		const U& get_second() const & noexcept {
			return static_cast<const base&>(*this).get_second();
		}

		U&& get_second() && noexcept {
            return static_cast<base&&>(*this).get_second();
		}

		template <typename U_ = U,
			std::enable_if_t<std::is_nothrow_move_constructible<U_>::value>* = nullptr>
		either_t& operator=(std::add_rvalue_reference_t<std::decay_t<U_>> u) noexcept {
			using opu = typename base::opu;
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), std::move(u));
			} else {
				opu::construct_at(std::addressof(this->_data.second), std::move(u));
				this->_state = either_state::second;
			}
			return *this;
		}

		template <typename U_ = U, std::enable_if_t<std::is_nothrow_copy_constructible<U_>::value>* = nullptr>
		either_t& operator=(std::add_lvalue_reference_t<std::decay_t<const U_>> u) noexcept {
			using opu = typename base::opu;
			if (!this->has_first()) {
				opu::emplace_at(std::addressof(this->_data.second), u);
			} else {
				opu::construct_at(std::addressof(this->_data.second), u);
				this->_state = either_state::second;
			}
			return *this;
		}

		using u_is_nothrow_move_constructible = std::true_type;
		using u_is_nothrow_copy_constructible = std::false_type;

		void swap_one_first_another_second_impl(either_t& rhs, u_is_nothrow_move_constructible) noexcept {
			using std::swap;

			base::opu::construct_at(std::addressof(this->_data.second), std::move(rhs._data.second));
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second_impl(either_t& rhs, u_is_nothrow_copy_constructible) noexcept {
			using std::swap;

			base::opu::construct_at(std::addressof(this->_data.second), rhs._data.second);
			this->_state = either_state::second;

			base::opu::destroy_at(std::addressof(rhs._data.second));
			rhs._state = either_state::first;
		}

		void swap_one_first_another_second(either_t& rhs) noexcept {
			swap_one_first_another_second_impl(rhs, std::is_nothrow_move_constructible<U>());
		}

		template <typename U_ = U,
			std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
				is_swappable<U_>::value
#else
				is_nothrow_swappable<U>::value
#endif
		>* =nullptr>
		void swap(either_t& rhs)
			noexcept(is_nothrow_swappable<U>::value) {
			if (this == std::addressof(rhs) || this->has_first() && rhs.has_first()) {
				return;
			}

			if (this->has_first() && !rhs.has_first()) {
				swap_one_first_another_second(rhs);
			} else if (!this->has_first() && rhs.has_first()) {
				rhs.swap(*this);
			} else {
				using std::swap;
				swap(this->get_second(), rhs.get_second());
			}
		}
	};

	template <typename T, typename U>
	void swap(either_t<T, U>& lhs, either_t<T, U>& rhs)
		noexcept(noexcept(std::declval<either_t<T, U>&>().swap(std::declval<either_t<T, U>&>()))) {
		lhs.swap(rhs);
	}

	template <typename T_, typename U_>
	constexpr bool operator==(const either_t<T_, U_>& lhs, const either_t<T_, U_>& rhs) noexcept {
		if (lhs.has_first() != rhs.has_first()) {
			return false;
		}

		return !lhs.has_first() ? lhs.get_second() == rhs.get_second()
							    : lhs.get_first() == rhs.get_first();
	}

	template <typename U_>
	constexpr bool operator==(const either_t<void, U_> &lhs, const either_t<void, U_> &rhs) noexcept {
		if (lhs.has_first() != rhs.has_first()) {
			return false;
		}

		return lhs.has_first() || lhs.get_second() == rhs.get_second();
	}

	template <typename T_, typename U_>
	constexpr bool operator!=(const either_t<T_, U_>& lhs, const either_t<T_, U_>& rhs) noexcept {
		return !(lhs == rhs);
	}
}

#endif
