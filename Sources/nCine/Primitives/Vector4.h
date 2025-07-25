#pragma once

#include "Vector2.h"
#include "Vector3.h"
#include "../../Main.h"

#include <cmath>

#include <Containers/Tags.h>

namespace nCine
{
	inline namespace Primitives
	{
		using Death::Containers::NoInitT;

		/// Four-component vector
		template<class T>
		class Vector4
		{
		public:
			T X, Y, Z, W;

			constexpr Vector4() noexcept
				: X{T(0)}, Y{T(0)}, Z{T(0)}, W{T(0)} {}
			explicit Vector4(NoInitT) noexcept {}
			explicit constexpr Vector4(T s) noexcept
				: X(s), Y(s), Z(s), W(s) {}
			constexpr Vector4(T x, T y, T z, T w) noexcept
				: X(x), Y(y), Z(z), W(w) {}
			constexpr Vector4(const Vector2<T>& other, T z, T w) noexcept
				: X(other.X), Y(other.Y), Z(z), W(w) {}
			constexpr Vector4(Vector2<T>&& other, T z, T w) noexcept
				: X(other.X), Y(other.Y), Z(z), W(w) {}
			constexpr Vector4(const Vector3<T>& other, T w) noexcept
				: X(other.X), Y(other.Y), Z(other.Z), W(w) {}
			constexpr Vector4(Vector3<T>&& other, T w) noexcept
				: X(other.X), Y(other.Y), Z(other.Z), W(w) {}
			constexpr Vector4(const Vector4& other) noexcept
				: X(other.X), Y(other.Y), Z(other.Z), W(other.W) {}
			constexpr Vector4(Vector4&& other) noexcept
				: X(other.X), Y(other.Y), Z(other.Z), W(other.W) {}
			Vector4& operator=(const Vector4& other) noexcept;
			Vector4& operator=(Vector4&& other) noexcept;

			void Set(T x, T y, T z, T w);

			T* Data();
			const T* Data() const;

			T& operator[](std::size_t index);
			const T& operator[](std::size_t index) const;

			bool operator==(const Vector4& v) const;
			bool operator!=(const Vector4& v) const;
			Vector4 operator-() const;

			Vector4& operator+=(const Vector4& v);
			Vector4& operator-=(const Vector4& v);
			Vector4& operator*=(const Vector4& v);
			Vector4& operator/=(const Vector4& v);

			Vector4& operator+=(T s);
			Vector4& operator-=(T s);
			Vector4& operator*=(T s);
			Vector4& operator/=(T s);

			Vector4 operator+(const Vector4& v) const;
			Vector4 operator-(const Vector4& v) const;
			Vector4 operator*(const Vector4& v) const;
			Vector4 operator/(const Vector4& v) const;

			Vector4 operator+(T s) const;
			Vector4 operator-(T s) const;
			Vector4 operator*(T s) const;
			Vector4 operator/(T s) const;

			template<class S>
			friend Vector4<S> operator*(S s, const Vector4<S>& v);

			T Length() const;
			T SqrLength() const;
			Vector4 Normalized() const;
			Vector4& Normalize();

			/// Converts elements of the vector to a specified type
			template<class S>
			Vector4<S> As() {
				return Vector4<S>(static_cast<S>(X), static_cast<S>(Y), static_cast<S>(Z), static_cast<S>(W));
			}

			Vector2<T> ToVector2() const;
			Vector3<T> ToVector3() const;

			static T Dot(const Vector4& v1, const Vector4& v2);
			static Vector4 Lerp(const Vector4& a, const Vector4& b, float t);

			/** @{ @name Constants */

			/// A vector with all zero elements
			static const Vector4 Zero;
			/// A unit vector on the X axis
			static const Vector4 XAxis;
			/// A unit vector on the Y axis
			static const Vector4 YAxis;
			/// A unit vector on the Z axis
			static const Vector4 ZAxis;
			/// A unit vector on the W axis
			static const Vector4 WAxis;

			/** @} */
		};

		/// Four-component vector of floats
		using Vector4f = Vector4<float>;
		/// Four-component vector of 32-bit integers
		using Vector4i = Vector4<int>;

		template<class T>
		inline Vector4<T>& Vector4<T>::operator=(const Vector4<T>& other) noexcept
		{
			X = other.X;
			Y = other.Y;
			Z = other.Z;
			W = other.W;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator=(Vector4<T>&& other) noexcept
		{
			X = other.X;
			Y = other.Y;
			Z = other.Z;
			W = other.W;

			return *this;
		}

		template<class T>
		inline void Vector4<T>::Set(T x, T y, T z, T w)
		{
			X = x;
			Y = y;
			Z = z;
			W = w;
		}

		template<class T>
		inline T* Vector4<T>::Data()
		{
			return &X;
		}

		template<class T>
		inline const T* Vector4<T>::Data() const
		{
			return &X;
		}

		template<class T>
		inline T& Vector4<T>::operator[](std::size_t index)
		{
			DEATH_ASSERT(index < 4);
			return (&X)[index];
		}

		template<class T>
		inline const T& Vector4<T>::operator[](std::size_t index) const
		{
			DEATH_ASSERT(index < 4);
			return (&X)[index];
		}

		template<class T>
		inline bool Vector4<T>::operator==(const Vector4& v) const
		{
			return (X == v.X && Y == v.Y && Z == v.Z && W == v.W);
		}

		template<class T>
		inline bool Vector4<T>::operator!=(const Vector4& v) const
		{
			return (X != v.X || Y != v.Y || Z != v.Z || W != v.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator-() const
		{
			return Vector4(-X, -Y, -Z, -W);
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator+=(const Vector4& v)
		{
			X += v.X;
			Y += v.Y;
			Z += v.Z;
			W += v.W;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator-=(const Vector4& v)
		{
			X -= v.X;
			Y -= v.Y;
			Z -= v.Z;
			W -= v.W;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator*=(const Vector4& v)
		{
			X *= v.X;
			Y *= v.Y;
			Z *= v.Z;
			W *= v.W;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator/=(const Vector4& v)
		{
			X /= v.X;
			Y /= v.Y;
			Z /= v.Z;
			W /= v.W;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator+=(T s)
		{
			X += s;
			Y += s;
			Z += s;
			W += s;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator-=(T s)
		{
			X -= s;
			Y -= s;
			Z -= s;
			W -= s;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator*=(T s)
		{
			X *= s;
			Y *= s;
			Z *= s;
			W *= s;

			return *this;
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::operator/=(T s)
		{
			X /= s;
			Y /= s;
			Z /= s;
			W /= s;

			return *this;
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator+(const Vector4& v) const
		{
			return Vector4(X + v.X,
						   Y + v.Y,
						   Z + v.Z,
						   W + v.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator-(const Vector4& v) const
		{
			return Vector4(X - v.X,
						   Y - v.Y,
						   Z - v.Z,
						   W - v.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator*(const Vector4& v) const
		{
			return Vector4(X * v.X,
						   Y * v.Y,
						   Z * v.Z,
						   W * v.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator/(const Vector4& v) const
		{
			return Vector4(X / v.X,
						   Y / v.Y,
						   Z / v.Z,
						   W / v.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator+(T s) const
		{
			return Vector4(X + s,
						   Y + s,
						   Z + s,
						   W + s);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator-(T s) const
		{
			return Vector4(X - s,
						   Y - s,
						   Z - s,
						   W - s);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator*(T s) const
		{
			return Vector4(X * s,
						   Y * s,
						   Z * s,
						   W * s);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::operator/(T s) const
		{
			return Vector4(X / s,
						   Y / s,
						   Z / s,
						   W / s);
		}

		template<class S>
		inline Vector4<S> operator*(S s, const Vector4<S>& v)
		{
			return Vector4<S>(s * v.X,
							  s * v.Y,
							  s * v.Z,
							  s * v.W);
		}

		template<class T>
		inline T Vector4<T>::Length() const
		{
			return (T)sqrt(X * X + Y * Y + Z * Z + W * W);
		}

		template<class T>
		inline T Vector4<T>::SqrLength() const
		{
			return X * X + Y * Y + Z * Z + W * W;
		}

		template<class T>
		inline Vector4<T> Vector4<T>::Normalized() const
		{
			const T len = Length();
			return Vector4(X / len, Y / len, Z / len, W / len);
		}

		template<class T>
		inline Vector4<T>& Vector4<T>::Normalize()
		{
			const T len = Length();

			X /= len;
			Y /= len;
			Z /= len;
			W /= len;

			return *this;
		}

		template<class T>
		inline Vector2<T> Vector4<T>::ToVector2() const
		{
			return Vector2<T>(X, Y);
		}

		template<class T>
		inline Vector3<T> Vector4<T>::ToVector3() const
		{
			return Vector3<T>(X, Y, Z);
		}

		template<class T>
		inline T Vector4<T>::Dot(const Vector4<T>& v1, const Vector4<T>& v2)
		{
			return static_cast<T>(v1.X * v2.X + v1.Y * v2.Y + v1.Z * v2.Z + v1.W * v2.W);
		}

		template<class T>
		inline Vector4<T> Vector4<T>::Lerp(const Vector4<T>& a, const Vector4<T>& b, float t)
		{
			return Vector4<T>(t * (b.X - a.X) + a.X, t * (b.Y - a.Y) + a.Y, t * (b.Z - a.Z) + a.Z, t * (b.W - a.W) + a.W);
		}

		template<class T>
		const Vector4<T> Vector4<T>::Zero(0, 0, 0, 0);
		template<class T>
		const Vector4<T> Vector4<T>::XAxis(1, 0, 0, 0);
		template<class T>
		const Vector4<T> Vector4<T>::YAxis(0, 1, 0, 0);
		template<class T>
		const Vector4<T> Vector4<T>::ZAxis(0, 0, 1, 0);
		template<class T>
		const Vector4<T> Vector4<T>::WAxis(0, 0, 0, 1);
	}
}
